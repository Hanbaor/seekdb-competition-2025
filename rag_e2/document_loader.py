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

        # Step 2: Check VLM cache first
        import json
        vlm_cached_pdfs = set()
        for pdf in pdf_images.keys():
            vlm_cache_file = self.working_dir / self.config["dataset"]["dataset_name"] / pdf.stem / "vlm_results.json"
            if vlm_cache_file.exists() and not rebuild:
                try:
                    with open(vlm_cache_file, 'r', encoding='utf-8') as f:
                        cached_results = json.load(f)
                    if len(cached_results) == len(pdf_images[pdf]):
                        vlm_cached_pdfs.add(pdf)
                        logger.info(f"VLM cache found for {pdf.name}, skipping VLM parsing")
                except:
                    pass
        
        # Enqueue Tasks (skip cached PDFs)
        task_queue = asyncio.Queue()
        pdf_metadata = {}
        for pdf, pages in pdf_images.items():
            pdf_metadata[pdf] = {
                "total_pages": len(pages),
                "marker": self.processed_dir / f"{pdf.stem}.done"
            }
            if pdf not in vlm_cached_pdfs:
                for page_num, image_path in pages:
                    await task_queue.put((pdf, page_num, image_path))
        
        logger.info(f"Enqueued {task_queue.qsize()} page tasks (skipped {len(vlm_cached_pdfs)} cached PDFs).")

        # Step 3: Workers
        page_results = defaultdict(dict)
        completed_pdfs = asyncio.Queue()
        failed_pages = []
        results_lock = asyncio.Lock()
        completed_pdf_set = set()
        vlm_workers_done = asyncio.Event()  # 标志：VLM workers 是否全部完成

        # Load VLM cache and mark completed PDFs
        for pdf in vlm_cached_pdfs:
            vlm_cache_file = self.working_dir / self.config["dataset"]["dataset_name"] / pdf.stem / "vlm_results.json"
            with open(vlm_cache_file, 'r', encoding='utf-8') as f:
                cached_results = json.load(f)
            # Convert string keys back to int
            page_results[pdf] = {int(k): v for k, v in cached_results.items()}
            completed_pdf_set.add(pdf)
            await completed_pdfs.put(pdf)
            logger.info(f"Loaded VLM cache for {pdf.name}: {len(page_results[pdf])} pages")

        # VLM Worker
        async def vlm_worker(worker_id):
            while True:
                try:
                    pdf_path, page_num, image_path = await asyncio.wait_for(task_queue.get(), timeout=1.0)
                except asyncio.TimeoutError:
                    if task_queue.empty():
                        break
                    continue
                
                # Skip if already cached
                async with results_lock:
                    if page_num in page_results[pdf_path]:
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
                            
                            # Save VLM results to cache
                            vlm_cache_dir = self.working_dir / self.config["dataset"]["dataset_name"] / pdf_path.stem
                            vlm_cache_dir.mkdir(parents=True, exist_ok=True)
                            vlm_cache_file = vlm_cache_dir / "vlm_results.json"
                            with open(vlm_cache_file, 'w', encoding='utf-8') as f:
                                # Convert int keys to string for JSON
                                json.dump({str(k): v for k, v in page_results[pdf_path].items()}, f, ensure_ascii=False, indent=2)
                            logger.info(f"Saved VLM cache for {pdf_path.name}")
                            
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
            logger.info(f"Graph Worker {worker_id} started")
            
            while True:
                try:
                    pdf_path = await asyncio.wait_for(completed_pdfs.get(), timeout=2.0)
                except asyncio.TimeoutError:
                    async with built_count_lock:
                        current_built = built_count
                    
                    # 如果已经处理完所有 PDF，可以退出
                    if current_built >= len(pdf_images):
                        logger.info(f"Graph Worker {worker_id} finished (all {len(pdf_images)} PDFs processed)")
                        break
                    
                    # 如果 VLM workers 已全部完成且队列为空，说明没有更多任务了
                    if vlm_workers_done.is_set() and completed_pdfs.empty():
                        logger.info(f"Graph Worker {worker_id} exiting (VLM done, queue empty, built={current_built}/{len(pdf_images)})")
                        break
                    
                    # 否则继续等待（VLM 可能还在处理）
                    continue
                
                try:
                    logger.info(f"Graph Worker {worker_id}: Building E2Graph for {pdf_path.name}...")
                    
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
                    # 注意：由于 Python GIL，CPU 密集型任务（spaCy NER）无法真正多线程并行
                    # 建议将 graph_worker_count 设置为 CPU 核心数（如 4-8）以获得最佳性能
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
        
        # 等待所有 VLM workers 完成
        await asyncio.gather(*vlm_tasks, return_exceptions=True)
        vlm_workers_done.set()  # 通知 graph workers：VLM 阶段已完成
        logger.info("All VLM workers completed")
        
        # 等待所有 graph workers 完成
        await asyncio.gather(*graph_tasks, return_exceptions=True)
        
        logger.info("Processing complete.")

    def _convert_pdfs_to_images(self, pdfs: List[Path]) -> Dict[Path, List]:
        pdf_images = {}
        total_pdfs = len(pdfs)
        for idx, pdf in enumerate(pdfs, 1):
            try:
                from pdf2image import convert_from_path
                output_dir = self.working_dir / pdf.stem / "vlm" / "images"
                output_dir.mkdir(parents=True, exist_ok=True)
                
                # Check if images already exist to save time
                existing_images = sorted(output_dir.glob("page_*.png"))
                if existing_images:
                     # Assuming if we have images, we're good. 
                     image_paths = []
                     for i, img_path in enumerate(existing_images, 1):
                         image_paths.append((i, img_path))
                     pdf_images[pdf] = image_paths
                     logger.info(f"[{idx}/{total_pdfs}] Skipped conversion (cached): {pdf.name}")
                     continue

                logger.info(f"[{idx}/{total_pdfs}] Converting {pdf.name}...")
                images = convert_from_path(str(pdf), dpi=150)
                image_paths = []
                for i, image in enumerate(images, 1):
                    img_path = output_dir / f"page_{i:04d}.png"
                    image.save(img_path, "PNG")
                    image_paths.append((i, img_path))
                pdf_images[pdf] = image_paths
                logger.info(f"  -> {len(images)} pages converted.")
            except Exception as e:
                logger.error(f"[{idx}/{total_pdfs}] Failed to convert {pdf}: {e}")
        return pdf_images

    def _run_e2graph_build(self, text, cache_folder):
        """
        Adapted from E2GraphRAG's parallel_build_extract.
        We call the underlying tasks directly or via a pool.
        """
        # Extract params from config
        cfg = self.config
        chunk_length = cfg["cluster"].get("chunk_length", 4000)  # 默认 4000 tokens
        overlap = cfg["cluster"]["overlap"]  # 默认 200
        merge_num = cfg["cluster"]["merge_num"]
        llm_path = cfg["llm"]["llm_path"]
        llm_device = cfg["llm"]["llm_device"]
        
        # 导入必要的工具
        import sys
        import os
        sys.path.insert(0, os.path.join(os.path.dirname(__file__), "E2GraphRAG-main"))
        from utils import sequential_split
        
        # 使用 tiktoken 代替 transformers tokenizer（避免中文 decode 加空格）
        import tiktoken
        tokenizer = tiktoken.encoding_for_model("gpt-4o-mini")  # 或 gpt-4, gpt-3.5-turbo
        
        # 按 token 切分文本
        logger.info(f"[TreeBuild] Splitting text with tiktoken, length={chunk_length}, overlap={overlap}")
        text_chunks = sequential_split(text, tokenizer, chunk_length, overlap)
        logger.info(f"[TreeBuild] Split into {len(text_chunks)} chunks")
        
        # Build Tree
        # Note: E2GraphRAG's build_tree_task takes a tuple of args
        build_args = (
            llm_path, llm_device, text_chunks, cache_folder, llm_path,
            chunk_length, overlap, merge_num, 
            "float16", # dtype placeholder
            "zh" # Assuming chinese based on previous context
        )
        
        # Extract Graph
        extract_args = (text_chunks, cache_folder, "zh")
        
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

            # 3. Save pickles for query (main.py expects .pkl)
            import pickle
            import json
            
            tree_json = Path(cache_folder) / "tree.json"
            graph_json = Path(cache_folder) / "graph.json"
            
            if tree_json.exists():
                with open(tree_json, "r") as f:
                    tree_data = json.load(f)
                with open(Path(cache_folder) / "tree.pkl", "wb") as f:
                    pickle.dump(tree_data, f)
                logger.info(f"Saved tree.pkl to {cache_folder}")

            if graph_json.exists():
                # graph.json is list of edges: [[u,v,w], ...]
                # We need G, index, appearance_count like E2GraphRAG pickle format
                # Load graph data
                with open(graph_json, "r") as f:
                    edges = json.load(f)
                
                # Reconstruct G
                import networkx as nx
                G = nx.Graph()
                for n1, n2, weight in edges:
                    G.add_edge(n1, n2, weight=weight)
                
                # Load index and counts
                index_path = Path(cache_folder) / "index.json"
                count_path = Path(cache_folder) / "appearance_count.json"
                
                index = {}
                appearance_count = {}
                if index_path.exists():
                    with open(index_path, "r") as f: index = json.load(f)
                if count_path.exists():
                    with open(count_path, "r") as f: appearance_count = json.load(f)

                with open(Path(cache_folder) / "graph.pkl", "wb") as f:
                    pickle.dump((G, index, appearance_count), f)
                logger.info(f"Saved graph.pkl to {cache_folder}")
            
        except Exception as e:
            logger.error(f"E2Graph build failed: {e}")
            raise

