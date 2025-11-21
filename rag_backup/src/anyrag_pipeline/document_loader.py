"""Document processing helpers for the RAGAnything pipeline."""

from __future__ import annotations

import asyncio
import os
from collections import defaultdict
from pathlib import Path
from typing import Iterable

from lightrag.utils import logger

from src.config import PipelineConfig
from src.anyrag_pipeline.raganything.raganything import RAGAnything


async def process_dataset(
    rag: RAGAnything,
    dataset_dir: str,
    rebuild: bool = False,
    parse_method: str | None = None,
) -> None:
    """Process every PDF in the dataset directory using page-level task queue."""

    dataset_path = Path(dataset_dir)
    if not dataset_path.exists():
        raise FileNotFoundError(f"Dataset directory not found: {dataset_dir}")

    pdf_files = sorted(p for p in dataset_path.glob("*.pdf"))
    if not pdf_files:
        logger.warning("No PDF files found in dataset directory")
        return

    processed_dir = Path(rag.working_dir) / "processed_documents"
    processed_dir.mkdir(parents=True, exist_ok=True)

    # Filter out cached PDFs
    pdfs_to_process = []
    for pdf in pdf_files:
        marker = processed_dir / f"{pdf.stem}.done"
        if not rebuild and marker.exists():
            logger.info(f"Skipping cached document: {pdf.name}")
        else:
            pdfs_to_process.append(pdf)

    if not pdfs_to_process:
        logger.info("All documents already processed (use --rebuild to reprocess)")
        return

    logger.info(f"Processing {len(pdfs_to_process)} PDFs from {dataset_dir}")
    
    # Ensure LightRAG is initialized before processing
    if rag.lightrag is None:
        logger.info("Initializing LightRAG for graph building...")
        await rag._ensure_lightrag_initialized()

    # Get worker count from environment or use default
    vlm_worker_count = int(os.getenv("VLM_WORKER_COUNT", "10"))
    graph_worker_count = int(os.getenv("GRAPH_WORKER_COUNT", "3"))
    logger.info(f"Using {vlm_worker_count} VLM workers for page-level processing")
    logger.info(f"Using {graph_worker_count} workers for parallel graph building")

    # Step 1: Convert all PDFs to images (batch preparation)
    logger.info("Step 1/3: Converting PDFs to images...")
    pdf_images = {}  # {pdf_path: [(page_num, image_path), ...]}
    
    for pdf in pdfs_to_process:
        try:
            # Use VLM parser's image conversion
            from pdf2image import convert_from_path
            
            output_dir = Path(rag.working_dir) / pdf.stem / "vlm" / "images"
            output_dir.mkdir(parents=True, exist_ok=True)
            
            images = convert_from_path(pdf, dpi=150)
            image_paths = []
            
            for i, image in enumerate(images, start=1):
                image_path = output_dir / f"page_{i:04d}.png"
                if not image_path.exists():
                    image.save(image_path, "PNG")
                image_paths.append((i, image_path))
            
            pdf_images[pdf] = image_paths
            logger.info(f"  Converted {pdf.name}: {len(image_paths)} pages")
            
        except Exception as e:
            logger.error(f"Failed to convert {pdf.name}: {e}")
            continue

    if not pdf_images:
        logger.error("No PDFs were successfully converted to images")
        return

    # Step 2: Create task queue and enqueue all pages (batch by batch)
    logger.info("Step 2/3: Enqueueing page tasks...")
    task_queue = asyncio.Queue()
    pdf_metadata = {}  # {pdf_path: {"total_pages": int, "marker": Path}}
    
    for pdf, pages in pdf_images.items():
        pdf_metadata[pdf] = {
            "total_pages": len(pages),
            "marker": processed_dir / f"{pdf.stem}.done",
        }
        
        for page_num, image_path in pages:
            await task_queue.put((pdf, page_num, image_path))
    
    total_tasks = task_queue.qsize()
    logger.info(f"  Enqueued {total_tasks} page tasks from {len(pdf_images)} PDFs")

    # Step 3: Process with worker pool
    logger.info(f"Step 3/3: Processing pages with {vlm_worker_count} workers...")
    
    # Result collectors
    page_results = defaultdict(dict)  # {pdf_path: {page_num: content_entry}}
    completed_pdfs = asyncio.Queue()  # Queue of completed PDFs ready for graph building
    failed_pages = []  # List of (pdf, page_num, error)
    results_lock = asyncio.Lock()  # Protect concurrent access to page_results
    completed_pdf_set = set()  # Track which PDFs have been marked complete
    
    # Worker function
    async def worker(worker_id: int):
        """Worker that processes page tasks from the queue."""
        processed_count = 0
        
        while True:
            try:
                pdf_path, page_num, image_path = await asyncio.wait_for(
                    task_queue.get(), timeout=1.0
                )
            except asyncio.TimeoutError:
                # Check if queue is truly empty
                if task_queue.empty():
                    break
                continue
            
            try:
                # Call VLM parser's single-page analysis
                content_entry = await rag.doc_parser._analyze_page_with_vlm(
                    image_path=image_path,
                    page_num=page_num,
                    vision_model_func=rag.vision_model_func,
                )
                
                # Store result and check completion under lock
                async with results_lock:
                    page_results[pdf_path][page_num] = content_entry
                    current_count = len(page_results[pdf_path])
                    total_count = pdf_metadata[pdf_path]["total_pages"]
                    
                    # Debug: log progress for first PDF
                    if pdf_path == list(pdf_metadata.keys())[0] and current_count % 5 == 0:
                        logger.info(f"  Progress: {pdf_path.name} - {current_count}/{total_count} pages")
                    
                    # Check if this PDF is complete (only once)
                    if (
                        pdf_path not in completed_pdf_set
                        and current_count == total_count
                    ):
                        completed_pdf_set.add(pdf_path)
                        await completed_pdfs.put(pdf_path)
                        logger.info(
                            f"  ✓ PDF complete: {pdf_path.name} "
                            f"({total_count} pages)"
                        )
                
                processed_count += 1
                
            except Exception as e:
                logger.error(
                    f"Worker-{worker_id} failed on {pdf_path.name} page {page_num}: {e}"
                )
                async with results_lock:
                    failed_pages.append((pdf_path, page_num, str(e)))
            
            finally:
                task_queue.task_done()
        
        logger.debug(f"Worker-{worker_id} finished, processed {processed_count} pages")

    # Graph builder function (并行处理)
    built_count_lock = asyncio.Lock()
    built_count = 0
    
    async def graph_builder(worker_id: int):
        """Build graphs for completed PDFs in parallel."""
        nonlocal built_count
        expected_count = len(pdf_images)
        processed = 0
        
        while True:
            try:
                pdf_path = await asyncio.wait_for(completed_pdfs.get(), timeout=2.0)
            except asyncio.TimeoutError:
                # Check if all PDFs are processed
                async with built_count_lock:
                    if built_count >= expected_count:
                        break
                # Check if there might be more PDFs coming
                if task_queue.empty() and completed_pdfs.empty():
                    # Wait a bit more in case PDFs are still being parsed
                    await asyncio.sleep(1)
                    if completed_pdfs.empty():
                        break
                continue
            
            try:
                # Sort pages and create content_list
                sorted_pages = sorted(page_results[pdf_path].keys())
                content_list = [page_results[pdf_path][p] for p in sorted_pages]
                
                logger.info(f"[GraphWorker-{worker_id}] Building graph for: {pdf_path.name}")
                
                # Generate base doc_id
                import hashlib
                combined_text = "".join(item["text"] for item in content_list)
                base_doc_id = f"doc-{hashlib.md5(combined_text.encode()).hexdigest()[:16]}"
                
                # ========== 关键优化：批量插入所有页面，让LightRAG内部并发处理 ==========
                logger.info(f"[LLM-EXTRACT-START] Worker-{worker_id} starting entity/relation extraction for {pdf_path.name} ({len(sorted_pages)} pages)")
                
                # 准备批量数据（list格式）
                batch_inputs = []
                batch_ids = []
                batch_file_paths = []
                
                for page_num, content_entry in zip(sorted_pages, content_list):
                    page_text = content_entry.get("text", "")
                    if not page_text.strip():
                        continue
                    
                    # 为每页生成唯一ID（包含页码）
                    page_doc_id = f"{base_doc_id}_page_{page_num}"
                    
                    # 在file_paths中编码页码信息（格式：filename#page=N）
                    file_path_with_page = f"{pdf_path.name}#page={page_num}"
                    
                    # 在文本中嵌入元数据标记
                    metadata_tag = f"[SOURCE:{pdf_path.name}|PAGE:{page_num}]"
                    tagged_text = f"{metadata_tag}\n{page_text}"
                    
                    # 收集到批量列表
                    batch_inputs.append(tagged_text)
                    batch_ids.append(page_doc_id)
                    batch_file_paths.append(file_path_with_page)
                
                # 一次性批量插入所有页面（LightRAG内部会并发处理chunks）
                logger.info(f"[LLM-EXTRACT] Worker-{worker_id} batch inserting {len(batch_inputs)} pages")
                await rag.lightrag.ainsert(
                    input=batch_inputs,  # ← list of strings
                    ids=batch_ids,       # ← list of ids
                    file_paths=batch_file_paths,  # ← list of file_paths
                )
                
                logger.info(f"[LLM-EXTRACT-DONE] Worker-{worker_id} finished extraction for {pdf_path.name} ({len(sorted_pages)} pages)")
                
                # Mark as complete and update count
                pdf_metadata[pdf_path]["marker"].touch()
                async with built_count_lock:
                    built_count += 1
                    current_count = built_count
                
                processed += 1
                logger.info(
                    f"  ✓ [GraphWorker-{worker_id}] Graph built for: {pdf_path.name} "
                    f"({current_count}/{expected_count} PDFs complete)"
                )
                
            except Exception as e:
                logger.exception(f"[GraphWorker-{worker_id}] Failed to build graph for {pdf_path.name}: {e}")
                async with built_count_lock:
                    built_count += 1  # Count as processed even if failed
        
        logger.info(f"[GraphWorker-{worker_id}] finished, built {processed} graphs")

    # Start worker pool and graph builders
    vlm_workers = [asyncio.create_task(worker(i)) for i in range(vlm_worker_count)]
    graph_builders = [asyncio.create_task(graph_builder(i)) for i in range(graph_worker_count)]
    
    # Wait for all VLM workers to complete
    await asyncio.gather(*vlm_workers, return_exceptions=True)
    
    # Wait for all graph builders to finish
    await asyncio.gather(*graph_builders, return_exceptions=True)
    
    # Summary
    logger.info("="*60)
    logger.info("Processing Summary:")
    logger.info(f"  Total PDFs: {len(pdf_images)}")
    logger.info(f"  Total pages processed: {sum(len(r) for r in page_results.values())}")
    logger.info(f"  Failed pages: {len(failed_pages)}")
    
    if failed_pages:
        logger.warning("Failed pages:")
        for pdf, page, error in failed_pages[:10]:  # Show first 10
            logger.warning(f"  - {pdf.name} page {page}: {error}")
    
    logger.info("="*60)
