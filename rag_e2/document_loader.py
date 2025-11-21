import asyncio
import os
import shutil
import logging
import hashlib
from pathlib import Path
from collections import defaultdict
from typing import List, Dict, Any
import multiprocessing as mp
import yaml

# Import E2GraphRAG components
try:
    from process_utils import build_tree_task, extract_graph_task, clean_cuda_memory
    from utils import Timer
except ImportError:
    pass

# Import local VLM parser
from vlm_parser import VLMParser

logger = logging.getLogger(__name__)

class DocumentProcessor:
    def __init__(self, config: Dict[str, Any], vlm_parser: VLMParser):
        self.config = config
        self.vlm_parser = vlm_parser
        self.working_dir = Path(config["paths"]["cache_path"])
        self.processed_dir = Path(config["paths"]["processed_docs_path"])
        self.working_dir.mkdir(parents=True, exist_ok=True)
        self.processed_dir.mkdir(parents=True, exist_ok=True)
        
        # Workers
        self.vlm_worker_count = config["vlm"]["worker_count"]
        self.graph_worker_count = config["graph"]["worker_count"]

    async def process_dataset(self, dataset_dir: str, rebuild: bool = False):
        """
        Process PDFs: Convert to Images -> VLM Parse -> E2GraphRAG Indexing
        """
        dataset_path = Path(dataset_dir)
        if not dataset_path.exists():
            raise FileNotFoundError(f"Dataset directory not found: {dataset_dir}")

        pdf_files = sorted(p for p in dataset_path.glob("*.pdf"))
        if not pdf_files:
            logger.warning("No PDF files found.")
            return

        # Filter cached
        pdfs_to_process = []
        for pdf in pdf_files:
            marker = self.processed_dir / f"{pdf.stem}.done"
            if not rebuild and marker.exists():
                logger.info(f"Skipping cached document: {pdf.name}")
            else:
                pdfs_to_process.append(pdf)

        if not pdfs_to_process:
            logger.info("All documents processed.")
            return

        logger.info(f"Processing {len(pdfs_to_process)} PDFs...")

        # Step 1: PDF -> Images
        logger.info("Step 1/3: Converting PDFs to images...")
        pdf_images = self._convert_pdfs_to_images(pdfs_to_process)
        
        if not pdf_images:
            logger.error("No images converted.")
            return

        # Step 2: Task Queue
        task_queue = asyncio.Queue()
        pdf_metadata = {}
        for pdf, pages in pdf_images.items():
            pdf_metadata[pdf] = {
                "total_pages": len(pages),
                "marker": self.processed_dir / f"{pdf.stem}.done"
            }
            for page_num, image_path in pages:
                await task_queue.put((pdf, page_num, image_path))
        
        logger.info(f"Enqueued {task_queue.qsize()} page tasks.")

        # Step 3: Workers
        page_results = defaultdict(dict)
        completed_pdfs = asyncio.Queue()
        failed_pages = []
        results_lock = asyncio.Lock()
        completed_pdf_set = set()

        # VLM Worker
        async def vlm_worker(worker_id):
            while True:
                try:
                    pdf_path, page_num, image_path = await asyncio.wait_for(task_queue.get(), timeout=1.0)
                except asyncio.TimeoutError:
                    if task_queue.empty():
                        break
                    continue
                
                try:
                    # Use local VLM parser
                    content_entry = await self.vlm_parser.analyze_page(
                        image_path=image_path,
                        page_num=page_num
                    )
                    
                    async with results_lock:
                        page_results[pdf_path][page_num] = content_entry
                        current = len(page_results[pdf_path])
                        total = pdf_metadata[pdf_path]["total_pages"]
                        
                        if pdf_path not in completed_pdf_set and current == total:
                            completed_pdf_set.add(pdf_path)
                            await completed_pdfs.put(pdf_path)
                            logger.info(f"PDF Ready for Graph: {pdf_path.name}")
                
                except Exception as e:
                    logger.error(f"VLM Worker {worker_id} failed on {pdf_path.name} p{page_num}: {e}")
                    async with results_lock:
                        failed_pages.append((pdf_path, page_num, str(e)))
                finally:
                    task_queue.task_done()

        # Graph Builder Worker (E2GraphRAG Integration)
        built_count_lock = asyncio.Lock()
        built_count = 0
        
        async def graph_worker(worker_id):
            nonlocal built_count
            while True:
                try:
                    pdf_path = await asyncio.wait_for(completed_pdfs.get(), timeout=2.0)
                except asyncio.TimeoutError:
                    async with built_count_lock:
                        if built_count >= len(pdf_images): break
                    if task_queue.empty() and completed_pdfs.empty():
                        await asyncio.sleep(1)
                        if completed_pdfs.empty(): break
                    continue
                
                try:
                    logger.info(f"Building E2Graph for {pdf_path.name}...")
                    
                    # Prepare text
                    sorted_pages = sorted(page_results[pdf_path].keys())
                    content_list = [page_results[pdf_path][p] for p in sorted_pages]
                    
                    # Combine text with page metadata for E2GraphRAG
                    # E2GraphRAG expects a single text string usually, but we want to preserve page info.
                    # We can join them with a delimiter or just concat. 
                    # For now, let's concat with page markers.
                    full_text = ""
                    for page_num, entry in zip(sorted_pages, content_list):
                        full_text += f"\n[PAGE_START:{page_num}]\n{entry['text']}\n[PAGE_END:{page_num}]\n"
                    
                    # Create cache folder specific to this PDF (like E2GraphRAG does for 'books')
                    doc_cache_dir = self.working_dir / self.config["dataset"]["dataset_name"] / pdf_path.stem
                    doc_cache_dir.mkdir(parents=True, exist_ok=True)
                    
                    # Call E2GraphRAG extraction logic
                    # NOTE: This is synchronous/blocking CPU/GPU work. 
                    # Ideally should run in a separate process to avoid blocking the event loop if we weren't already in a worker.
                    # However, since we are in an async worker, we should offload this to a thread or process if possible.
                    # But E2GraphRAG uses multiprocessing internally. calling it directly might be okay if we don't starve the loop.
                    # Let's run it in a thread to be safe, or just call it directly if we trust the overlap.
                    
                    # We'll adapt 'parallel_build_extract' logic here
                    await asyncio.to_thread(
                        self._run_e2graph_build,
                        full_text,
                        str(doc_cache_dir)
                    )
                    
                    # Mark done
                    pdf_metadata[pdf_path]["marker"].touch()
                    async with built_count_lock:
                        built_count += 1
                    
                    logger.info(f"Finished E2Graph for {pdf_path.name}")

                except Exception as e:
                    logger.error(f"Graph Worker {worker_id} failed on {pdf_path.name}: {e}")
                    # Count it anyway to avoid infinite wait
                    async with built_count_lock:
                        built_count += 1
        
        # Start workers
        vlm_tasks = [asyncio.create_task(vlm_worker(i)) for i in range(self.vlm_worker_count)]
        graph_tasks = [asyncio.create_task(graph_worker(i)) for i in range(self.graph_worker_count)]
        
        await asyncio.gather(*vlm_tasks, return_exceptions=True)
        await asyncio.gather(*graph_tasks, return_exceptions=True)
        
        logger.info("Processing complete.")

    def _convert_pdfs_to_images(self, pdfs: List[Path]) -> Dict[Path, List]:
        pdf_images = {}
        for pdf in pdfs:
            try:
                from pdf2image import convert_from_path
                output_dir = self.working_dir / pdf.stem / "vlm" / "images"
                output_dir.mkdir(parents=True, exist_ok=True)
                
                # Check if images already exist to save time
                existing_images = sorted(output_dir.glob("page_*.png"))
                if existing_images:
                     # Assuming if we have images, we're good. 
                     # In a robust system we'd check count matches PDF pages.
                     image_paths = []
                     for i, img_path in enumerate(existing_images, 1):
                         image_paths.append((i, img_path))
                     pdf_images[pdf] = image_paths
                     continue

                images = convert_from_path(str(pdf), dpi=150)
                image_paths = []
                for i, image in enumerate(images, 1):
                    img_path = output_dir / f"page_{i:04d}.png"
                    image.save(img_path, "PNG")
                    image_paths.append((i, img_path))
                pdf_images[pdf] = image_paths
            except Exception as e:
                logger.error(f"Failed to convert {pdf}: {e}")
        return pdf_images

    def _run_e2graph_build(self, text, cache_folder):
        """
        Adapted from E2GraphRAG's parallel_build_extract.
        We call the underlying tasks directly or via a pool.
        """
        # Extract params from config
        cfg = self.config
        length = cfg["cluster"]["length"]
        overlap = cfg["cluster"]["overlap"]
        merge_num = cfg["cluster"]["merge_num"]
        llm_path = cfg["llm"]["llm_path"]
        llm_device = cfg["llm"]["llm_device"]
        
        # Since we are likely running this inside an asyncio loop which might be in a thread,
        # we need to be careful with multiprocessing.
        # E2GraphRAG uses mp.Pool. Daemon processes can't have children.
        # We will try to call the tasks sequentially here to avoid complexity, 
        # or use a fresh pool if we are in the main process.
        # For simplicity and stability in this migration, let's run them sequentially 
        # but potentially they are internally parallelized if implemented right.
        
        # Actually, E2GraphRAG's main benefit is that build_tree and extract_graph run in parallel.
        # We can simulate that with threads or just run them.
        
        # Build Tree
        # Note: E2GraphRAG's build_tree_task takes a tuple of args
        build_args = (
            llm_path, llm_device, text, cache_folder, llm_path,
            length, overlap, merge_num, 
            "float16", # dtype placeholder
            "zh" # Assuming chinese based on previous context
        )
        
        # Extract Graph
        extract_args = (text, cache_folder, "zh")
        
        # Running them purely sequentially for safety in this 'backup-to-e2' migration
        # unless we confirm we can spawn processes safely.
        try:
            # We need to import these inside the method or ensure they are available
            from process_utils import build_tree_task, extract_graph_task
            
            # 1. Build Tree
            logger.info(f"Starting build_tree_task for {Path(cache_folder).name}")
            build_tree_task(build_args)
            
            # 2. Extract Graph
            logger.info(f"Starting extract_graph_task for {Path(cache_folder).name}")
            extract_graph_task(extract_args)
            
        except Exception as e:
            logger.error(f"E2Graph build failed: {e}")
            raise

