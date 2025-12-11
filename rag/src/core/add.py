import glob
import logging
import os
import uuid
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import List, Dict, Tuple

import dotenv
from pyobvector import VECTOR, FtsIndexParam, FtsParser, VectorIndex
from pyobvector.client.hybrid_search import HybridSearch
from sqlalchemy import VARCHAR, Column, Integer

logger = logging.getLogger(__name__)

try:
    from pypdf import PdfReader
except ImportError:
    try:
        from PyPDF2 import PdfReader
    except ImportError:
        PdfReader = None

from src.integrations.embedding import generate_response as generate_embedding
from src.integrations.vlm import generate_response as generate_vlm_response
from src.parser.pdf import compress_image, pdf_page_to_image
from src.prompt.vlm import VLM_IMAGE_EXTRACTION_PROMPT
from src.split.split import split_text
from src.storage.oceanbase import get_or_create_client

dotenv.load_dotenv()

# Embedding dimension - must match the model in embedding/openai_client.py
EMBEDDING_DIM = 2048

# Table name for storing document chunks
TABLE_NAME = "rag_documents"



from sqlalchemy import VARCHAR, Column, Integer


def recreate_table(client: HybridSearch, vector_dim: int, retry_interval: float = 2.0):
    """Drop and recreate the documents table with infinite retry."""
    
    # Step 1: Drop existing table (with retry)
    logger.info(f"Dropping table '{TABLE_NAME}' if exists...")
    attempt = 0
    while True:
        attempt += 1
        try:
            client.drop_table_if_exist(table_name=TABLE_NAME)
            logger.info(f"Table '{TABLE_NAME}' dropped (or didn't exist).")
            break
        except Exception as e:
            logger.warning(f"Failed to drop table (attempt {attempt}): {e}. Retrying in {retry_interval}s...")
            time.sleep(retry_interval)

    # Step 2: Create new table (with retry)
    logger.info(f"Creating table '{TABLE_NAME}'...")
    attempt = 0
    while True:
        attempt += 1
        try:
            client.create_table(
                table_name=TABLE_NAME,
                columns=[
                    Column("id", Integer, primary_key=True, autoincrement=True),
                    Column("source_id", VARCHAR(64)),  # Unique identifier for each chunk
                    Column("filename", VARCHAR(512)),  # PDF filename
                    Column("page", Integer),  # Page number (1-indexed)
                    Column("content", VARCHAR(8192)),  # Text content
                    Column("vector", VECTOR(vector_dim)),  # Embedding vector
                ],
                indexes=[
                    VectorIndex("vec_idx", "vector", params="distance=l2, type=hnsw, lib=vsag"),
                ],
                mysql_charset="utf8mb4",
                mysql_collate="utf8mb4_unicode_ci",
                mysql_organization="heap",
            )
            
            # Create full-text search index on content
            client.create_fts_idx_with_fts_index_param(
                table_name=TABLE_NAME,
                fts_idx_param=FtsIndexParam(
                    index_name="fts_idx_content",
                    field_names=["content"],
                    parser_type=FtsParser.IK,
                ),
            )
            logger.info(f"Table '{TABLE_NAME}' created successfully.")
            return
            
        except Exception as e:
            logger.warning(f"Error creating table (attempt {attempt}): {e}. Retrying in {retry_interval}s...")
            time.sleep(retry_interval)

# ... process_pdf_file ...

def add(dataset_dir: str, max_worker: int):
    """
    Prepare RAG data by parsing PDFs with page-level concurrency.
    
    Args:
        dataset_dir: Directory containing PDF files
        max_worker: Number of parallel workers for page-level processing
    """
    # Find all PDF files
    pdf_files: List[str] = glob.glob(os.path.join(dataset_dir, "*.pdf"))
    
    if not pdf_files:
        logger.warning(f"No PDF files found in {dataset_dir}")
        return
    
    logger.info(f"Found {len(pdf_files)} PDF files")
    
    # Initialize OceanBase client
    client = get_or_create_client()

    # Use embedding dimension from environment variable
    vector_dim = EMBEDDING_DIM
    
    # Recreate the table from scratch every time it runs
    recreate_table(client, vector_dim)

    # Process all PDF files
    files_to_process = pdf_files
    logger.info(f"Processing {len(files_to_process)} PDF files...")
    
    # [Modified] Create page-level task queue for finer-grained parallelism
    page_tasks = []  # List of (pdf_path, page_num, total_pages)
    pdf_total_pages = {}  # {pdf_path: total_pages}
    
    for pdf_file in files_to_process:
        try:
            if PdfReader is None:
                raise ImportError("PdfReader not available")
            reader = PdfReader(pdf_file)
            total_pages = len(reader.pages)
            pdf_total_pages[pdf_file] = total_pages
            
            for page_num in range(1, total_pages + 1):
                page_tasks.append((pdf_file, page_num, total_pages))
        except Exception as e:
            logger.error(f"Failed to read PDF {os.path.basename(pdf_file)}: {e}")
            continue
    
    logger.info(f"Created {len(page_tasks)} page-level tasks from {len(files_to_process)} PDFs")
    
    # Track chunks for each PDF (for batch insertion)
    pdf_chunks = defaultdict(list)  # {pdf_path: [chunk1, chunk2, ...]}
    pdf_completed_pages = defaultdict(int)  # {pdf_path: completed_page_count}
    
    total_chunks = 0
    completed_pdfs = 0
    
    # Process pages in parallel (page-level concurrency)
    with ThreadPoolExecutor(max_workers=max_worker) as executor:
        # Submit all page tasks
        future_to_task = {
            executor.submit(process_single_page, pdf_path, page_num, client): (pdf_path, page_num, total_pages)
            for pdf_path, page_num, total_pages in page_tasks
        }
        
        # Collect results as they complete
        for future in as_completed(future_to_task):
            pdf_path, page_num, total_pages = future_to_task[future]
            filename = os.path.basename(pdf_path)
            
            try:
                page_chunks = future.result()
                
                # Add to PDF's chunk collection
                pdf_chunks[pdf_path].extend(page_chunks)
                pdf_completed_pages[pdf_path] += 1
                
                logger.debug(f"{filename} page {page_num}/{total_pages} completed, {len(page_chunks)} chunks")
                
                # Check if this PDF is fully processed
                if pdf_completed_pages[pdf_path] >= total_pages:
                    # All pages of this PDF completed, batch insert
                    chunks_to_insert = pdf_chunks[pdf_path]
                    
                    if chunks_to_insert:
                        # Insert in batches
                        batch_size = 100
                        inserted = 0
                        for i in range(0, len(chunks_to_insert), batch_size):
                            batch = chunks_to_insert[i : i + batch_size]
                            client.insert(table_name=TABLE_NAME, data=batch)
                            inserted += len(batch)
                        
                        total_chunks += inserted
                        completed_pdfs += 1
                        logger.info(
                            f"✓ Completed {filename}: {inserted} chunks inserted "
                            f"({completed_pdfs}/{len(files_to_process)} PDFs done)"
                        )
                    
                    # Free memory
                    del pdf_chunks[pdf_path]
                    del pdf_completed_pages[pdf_path]
                    
            except Exception as e:
                logger.error(f"Failed to process {filename} page {page_num}: {e}", exc_info=True)
                continue
    
    logger.info(f"Data preparation completed. Total chunks inserted: {total_chunks}")



def process_single_page(
    pdf_file: str,
    page_num: int,
    client: HybridSearch,
) -> List[Dict]:
    """
    Process a single PDF page with workflow: PDF to image -> VLM extraction -> Split -> Embed
    
    Args:
        pdf_file: Path to the PDF file
        page_num: Page number to process (1-indexed)
        client: OceanBase client (not used but kept for compatibility)
    
    Returns:
        List of chunk dictionaries ready for insertion
    """
    filename = os.path.basename(pdf_file)
    page_image_path = None
    compressed_image_path = None
    page_text = ""
    chunks_list = []
    
    try:
        # Step 1: Convert PDF page to image
        page_image_path = pdf_page_to_image(pdf_file, page_num, dpi=100)
        logger.debug(f"{filename} page {page_num}: converted to image")
        
        # Step 2: Extract information using VLM
        try:
            page_text = generate_vlm_response(
                VLM_IMAGE_EXTRACTION_PROMPT, images=page_image_path
            )
            if page_text.strip():
                logger.debug(f"{filename} page {page_num}: extracted text via VLM")
        except Exception as e:
            # Check if it's a 502 error (likely due to large image)
            error_str = str(e).lower()
            is_502_error = (
                "502" in error_str
                or "bad gateway" in error_str
                or (hasattr(e, "status_code") and e.status_code == 502)
            )
            
            if is_502_error:
                logger.warning(
                    f"{filename} page {page_num}: 502 error, attempting compression..."
                )
                try:
                    # Compress the image and retry
                    compressed_image_path = compress_image(
                        page_image_path,
                        max_size_mb=5.0,
                        max_dimension=2048,
                    )
                    page_text = generate_vlm_response(
                        VLM_IMAGE_EXTRACTION_PROMPT,
                        images=compressed_image_path,
                    )
                    if page_text.strip():
                        logger.info(f"{filename} page {page_num}: extracted via compressed image")
                except Exception as retry_e:
                    logger.warning(
                        f"{filename} page {page_num}: failed after compression: {retry_e}"
                    )
            else:
                logger.warning(f"{filename} page {page_num}: VLM extraction failed: {e}")
    except Exception as e:
        logger.warning(f"{filename} page {page_num}: processing failed: {e}")
    finally:
        # Clean up temporary image files
        if compressed_image_path and os.path.exists(compressed_image_path):
            try:
                os.unlink(compressed_image_path)
            except Exception as e:
                logger.debug(f"Failed to delete compressed image: {e}")
        if page_image_path and os.path.exists(page_image_path):
            try:
                os.unlink(page_image_path)
            except Exception as e:
                logger.debug(f"Failed to delete page image: {e}")
    
    # Skip if no content extracted
    if not page_text.strip():
        logger.debug(f"{filename} page {page_num}: no content, skipping")
        return chunks_list
    
    # Step 3: Split text
    text_chunks = split_text(page_text)
    
    # Step 4 & 5: Embed -> Prepare for storage
    for chunk_text in text_chunks:
        if not chunk_text.strip():
            continue
        
        # Truncate if too long before calling embedding API
        if len(chunk_text) > 8190:
            logger.warning(
                f"Chunk from {filename} page {page_num} is too long ({len(chunk_text)} chars), "
                f"truncating to 8190 chars before embedding"
            )
            chunk_text = chunk_text[:8190]
        
        # Generate embedding with 2048 dimensions
        try:
            embedding = generate_embedding(chunk_text, dimensions=2048)
        except Exception as e:
            logger.warning(
                f"Failed to generate embedding for chunk in {filename} page {page_num}: {e}"
            )
            continue
        
        # Create unique source_id
        source_id = str(uuid.uuid4())
        
        chunks_list.append(
            {
                "source_id": source_id,
                "filename": filename,
                "page": page_num,
                "content": chunk_text[:8190],  # Truncate if too long
                "vector": embedding,
            }
        )
    
    return chunks_list


def process_pdf_file(
    pdf_file: str,
    client: HybridSearch,
) -> int:
    """
    [DEPRECATED] Legacy function kept for backward compatibility.
    Use page-level processing in add() instead.
    
    Process a single PDF file with fixed workflow for each page:
    PDF to image -> VLM extraction -> Split -> Embed -> Store

    Args:
        pdf_file: Path to the PDF file
        client: OceanBase client for database operations

    Returns:
        Number of chunks inserted
    """
    filename = os.path.basename(pdf_file)
    logger.info(f"Processing {filename}...")

    try:
        # Get total number of pages from PDF
        try:
            if PdfReader is None:
                raise ImportError("PdfReader not available")
            reader = PdfReader(pdf_file)
            total_pages = len(reader.pages)
        except Exception as e:
            logger.error(f"Failed to read PDF {filename}: {e}")
            return 0

        logger.debug(f"  Total pages: {total_pages}")

        # Process each page with fixed workflow
        chunks_to_insert = []
        for page_num in range(1, total_pages + 1):
            page_image_path = None
            compressed_image_path = None
            page_text = ""

            try:
                # Step 1: Convert PDF to image
                page_image_path = pdf_page_to_image(pdf_file, page_num, dpi=100)
                logger.debug(f"Converted page {page_num} to image: {page_image_path}")

                # Step 2: Extract information using VLM
                try:
                    page_text = generate_vlm_response(
                        VLM_IMAGE_EXTRACTION_PROMPT, images=page_image_path
                    )
                    if page_text.strip():
                        logger.debug(f"Extracted text from page {page_num} using VLM")
                except Exception as e:
                    # Check if it's a 502 error (likely due to large image)
                    error_str = str(e).lower()
                    is_502_error = (
                        "502" in error_str
                        or "bad gateway" in error_str
                        or (hasattr(e, "status_code") and e.status_code == 502)
                    )

                    if is_502_error:
                        logger.warning(
                            f"502 error detected for page {page_num} of {filename}. "
                            f"Attempting to compress image and retry..."
                        )
                        try:
                            # Compress the image and retry
                            compressed_image_path = compress_image(
                                page_image_path,
                                max_size_mb=5.0,
                                max_dimension=2048,
                            )
                            page_text = generate_vlm_response(
                                VLM_IMAGE_EXTRACTION_PROMPT,
                                images=compressed_image_path,
                            )
                            if page_text.strip():
                                logger.info(
                                    f"Successfully extracted text from compressed page {page_num} image"
                                )
                        except Exception as retry_e:
                            logger.warning(
                                f"Failed to extract text from compressed page {page_num} image of {filename}: {retry_e}"
                            )
                    else:
                        logger.warning(
                            f"Failed to extract text from page {page_num} image of {filename}: {e}"
                        )
            except Exception as e:
                logger.warning(f"Failed to process page {page_num} of {filename}: {e}")
            finally:
                # Clean up temporary image files
                if compressed_image_path and os.path.exists(compressed_image_path):
                    try:
                        os.unlink(compressed_image_path)
                    except Exception as e:
                        logger.debug(
                            f"Failed to delete compressed image {compressed_image_path}: {e}"
                        )
                if page_image_path and os.path.exists(page_image_path):
                    try:
                        os.unlink(page_image_path)
                    except Exception as e:
                        logger.debug(
                            f"Failed to delete temporary page image {page_image_path}: {e}"
                        )

            # Skip if no content extracted
            if not page_text.strip():
                logger.debug(f"No content extracted from page {page_num}, skipping")
                continue

            # Step 3: Split text
            text_chunks = split_text(page_text)

            # Step 4 & 5: Embed -> Store
            for chunk_text in text_chunks:
                if not chunk_text.strip():
                    continue

                # Truncate if too long before calling embedding API
                if len(chunk_text) > 8190:
                    logger.warning(
                        f"Chunk from {filename} page {page_num} is too long ({len(chunk_text)} chars), "
                        f"truncating to 8190 chars before embedding"
                    )
                    chunk_text = chunk_text[:8190]

                # Generate embedding with 2048 dimensions
                try:
                    embedding = generate_embedding(chunk_text, dimensions=2048)
                except Exception as e:
                    logger.warning(
                        f"Failed to generate embedding for chunk in {filename} page {page_num}: {e}"
                    )
                    continue

                # Create unique source_id
                source_id = str(uuid.uuid4())

                chunks_to_insert.append(
                    {
                        "source_id": source_id,
                        "filename": filename,
                        "page": page_num,
                        "content": chunk_text[:8190],  # Truncate if too long
                        "vector": embedding,
                    }
                )

        # Batch insert chunks
        chunks_inserted = 0
        if chunks_to_insert:
            # Insert in batches to avoid memory issues
            batch_size = 100
            for i in range(0, len(chunks_to_insert), batch_size):
                batch = chunks_to_insert[i : i + batch_size]
                client.insert(table_name=TABLE_NAME, data=batch)
                chunks_inserted += len(batch)
                logger.debug(
                    f"Inserted {len(batch)} chunks from {filename} (total: {chunks_inserted})"
                )
            logger.info(
                f"Completed processing {filename}: inserted {chunks_inserted} chunks"
            )

        return chunks_inserted

    except Exception as e:
        logger.error(f"Error processing {filename}: {e}", exc_info=True)
        return 0


# Old add() function removed - using new page-level concurrent version above
