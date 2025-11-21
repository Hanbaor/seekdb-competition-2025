import argparse
import asyncio
import logging
import os
import sys
import json
import yaml
from pathlib import Path
import torch
from dotenv import load_dotenv

# Load environment variables
load_dotenv()

# Add E2GraphRAG to path
PROJECT_ROOT = Path(__file__).resolve().parent
E2_PATH = PROJECT_ROOT / "E2GraphRAG-main"
if str(E2_PATH) not in sys.path:
    sys.path.insert(0, str(E2_PATH))

# Add rag_backup/src to path for VLM parser reuse if needed
BACKUP_SRC_PATH = PROJECT_ROOT.parent / "rag_backup" / "src"
if str(BACKUP_SRC_PATH) not in sys.path:
    sys.path.insert(0, str(BACKUP_SRC_PATH))

from document_loader import DocumentProcessor
from api_client import APIClient
from vlm_parser import VLMParser

# Import E2GraphRAG query components
try:
    from query import Retriever
    from extract_graph import load_nlp
    from prompt_dict import Prompts
except ImportError:
    print("Warning: Failed to import E2GraphRAG components. Make sure E2GraphRAG-main is in the directory.")

logger = logging.getLogger(__name__)

# Reduce noise from dependency loggers (httpx/OpenAI clients)
logging.getLogger("httpx").setLevel(logging.WARNING)
logging.getLogger("openai").setLevel(logging.WARNING)

def load_config(config_path):
    with open(config_path, "r") as f:
        return yaml.safe_load(f)

import re

def extract_source_info(context_text, doc_name):
    """
    Extract filename and page number from context text.
    Assumes format: [PAGE_START:num] ... content ...
    """
    # Find all page markers
    matches = re.findall(r"\[PAGE_START:(\d+)\]", context_text)
    
    page_num = 0
    if matches:
        # Use the most frequent page or the first one?
        # Let's use the first one found in the top chunk
        try:
            page_num = int(matches[0])
        except ValueError:
            pass
            
    # Construct filename (assuming doc_name is stem)
    # Competition often requires full filename.
    # We only have the stem from cache dir name.
    # If the original file was .pdf, we append it.
    filename = f"{doc_name}.pdf"
    
    return filename, page_num

async def main_async(args):
    config = load_config(args.config)
    
    # 1. Initialize VLM Context
    api_client = APIClient(config)
    vlm_parser = VLMParser(vision_model_func=api_client.call_vlm)
    
    # 2. Process Documents
    processor = DocumentProcessor(config, vlm_parser)
    if not args.skip_ingest:
        await processor.process_dataset(
            config["dataset"]["dataset_path"], 
            rebuild=args.rebuild
        )
    
    # 3. Query / Evaluation
    if args.questions:
        with open(args.questions, 'r') as f:
            # rag_backup questions.json is a list of strings
            questions = json.load(f) 
        
        if not isinstance(questions, list):
            logger.error("Questions file must contain a list of strings or dicts")
            return

        # Load NLP once
        nlp = load_nlp()
        
        # Identify all processed docs
        cache_root = Path(config["paths"]["cache_path"]) / config["dataset"]["dataset_name"]
        cache_root.mkdir(parents=True, exist_ok=True)
        doc_dirs = sorted([d for d in cache_root.iterdir() if d.is_dir()])
        
        # We need to load the Graph/Tree for each doc to build retrievers
        # Since we can't keep 100 graphs in memory easily if they are huge, maybe we load one by one?
        # But for 14 questions, iterating docs for each question is O(Q*D).
        # Loading graph takes time.
        # Better: Load all Retrievers into memory (if they fit).
        
        retrievers = []
        doc_names = []
        
        for doc_dir in doc_dirs:
            try:
                # Load cached data
                # E2GraphRAG caches: 'tree.pkl', 'graph.pkl' (G, index, appearance_count)
                import pickle
                
                tree_path = doc_dir / "tree.pkl"
                graph_path = doc_dir / "graph.pkl"
                
                if not tree_path.exists() or not graph_path.exists():
                    logger.warning(f"Skipping incomplete cache: {doc_dir}")
                    continue
                
                with open(tree_path, "rb") as f:
                    tree = pickle.load(f)
                
                with open(graph_path, "rb") as f:
                    G, index, appearance_count = pickle.load(f)
                
                # Initialize Retriever
                retriever = Retriever(
                    tree, G, index, appearance_count, nlp, 
                    **config["retriever"]["kwargs"]
                )
                retrievers.append(retriever)
                doc_names.append(doc_dir.name)
                logger.info(f"Loaded retriever for {doc_dir.name}")
                
            except Exception as e:
                logger.error(f"Failed to load {doc_dir.name}: {e}")

        # 并行查询配置
        max_concurrent = config.get("query", {}).get("max_concurrent_queries", 3)
        semaphore = asyncio.Semaphore(max_concurrent)
        api_client = APIClient(config)
        
        async def process_one_question(idx, question_item):
            """处理单个问题（检索+生成答案）"""
            async with semaphore:  # 控制并发数
                # Handle both string and dict formats
                if isinstance(question_item, dict):
                    question = question_item.get("question", "")
                else:
                    question = str(question_item)
                    
                logger.info(f"[{idx}/{len(questions)}] Processing: {question}")
                
                all_chunks = []
                scored_results = []
                debug_info = {"retrievals": []}
                
                # Retrieve from all docs
                for i, retriever in enumerate(retrievers):
                    try:
                        result = retriever.query(question, **config["retriever"]["kwargs"])
                        
                        # 计算检索得分（基于返回的chunk数量）
                        chunk_ids = result.get("chunk_ids", {})
                        total_chunks = sum(len(ids) for ids in chunk_ids.values())
                        retrieval_score = total_chunks if total_chunks > 0 else 0
                        
                        chunks_text = result.get("chunks", "").strip()
                        
                        # 记录检索详情
                        retrieval_detail = {
                            "doc_name": doc_names[i],
                            "doc_index": i,
                            "retrieval_type": result.get("retrieval_type", "unknown"),
                            "entities": result.get("entities", []),
                            "chunk_ids": result.get("chunk_ids", {}),
                            "chunks_text": chunks_text,
                            "chunks_length": len(chunks_text),
                            "has_result": bool(chunks_text),
                            "retrieval_score": retrieval_score
                        }
                        debug_info["retrievals"].append(retrieval_detail)
                        
                        if chunks_text:
                            all_chunks.append(f"Source: {doc_names[i]}\n{chunks_text}")
                            scored_results.append({
                                "doc_name": doc_names[i],
                                "doc_index": i,
                                "chunks": chunks_text,
                                "score": retrieval_score,
                                "chunks_length": len(chunks_text)
                            })
                    except Exception as e:
                        logger.error(f"Query failed for {doc_names[i]}: {e}")
                        debug_info["retrievals"].append({
                            "doc_name": doc_names[i],
                            "doc_index": i,
                            "error": str(e)
                        })
                
                # 生成答案 - 使用得分最高的文档
                best_context = ""
                if scored_results:
                    # 按得分排序，使用得分最高的文档
                    scored_results.sort(key=lambda x: (x["score"], x["chunks_length"]), reverse=True)
                    best_context = f"Source: {scored_results[0]['doc_name']}\n{scored_results[0]['chunks']}"
                elif all_chunks:
                    best_context = all_chunks[0]
                
                prompt = f"""Based on the following context, answer the question.
If the answer is not in the context, say "I don't know".

Context:
{best_context}

Question: {question}
Answer:"""

                messages = [{"role": "user", "content": prompt}]
                answer_text = await api_client.call_vlm(messages)
                
                # Format for output - 使用得分最高的文档
                best_filename = ""
                best_page = 0
                
                # 选择得分最高的文档作为来源（已经排序过了）
                if scored_results:
                    best_result = scored_results[0]
                    
                    try:
                        doc_name = best_result["doc_name"]
                        chunks_text = best_result["chunks"]
                        best_filename, best_page = extract_source_info(chunks_text, doc_name)
                        
                        # 记录选择的依据
                        debug_info["best_source_selection"] = {
                            "selected_doc": doc_name,
                            "score": best_result["score"],
                            "rank": 1,
                            "total_candidates": len(scored_results)
                        }
                    except Exception as e:
                        logger.error(f"Failed to extract source info: {e}")
                
                debug_info["source"] = {"filename": best_filename, "page": best_page}
                debug_info["question"] = question

                logger.info(f"[{idx}/{len(questions)}] Completed: {question[:50]}...")
                
                # 保存debug信息
                debug_file = f"debug_output/question_{idx:02d}.json"
                with open(debug_file, 'w', encoding='utf-8') as f:
                    json.dump(debug_info, f, indent=2, ensure_ascii=False)
                logger.info(f"  (debug saved to {debug_file})")
                
                return {
                    "question": question,
                    "answer": answer_text,
                    "filename": best_filename,
                    "page": best_page
                }
        
        # 并行执行所有问题
        logger.info(f"Starting parallel query with max_concurrent={max_concurrent}")
        tasks = [process_one_question(i+1, q) for i, q in enumerate(questions)]
        answers = await asyncio.gather(*tasks, return_exceptions=True)
        
        # 过滤掉异常结果
        valid_answers = []
        for i, result in enumerate(answers):
            if isinstance(result, Exception):
                logger.error(f"Question {i+1} failed: {result}")
                # 添加错误占位
                valid_answers.append({
                    "question": questions[i].get("question", str(questions[i])) if isinstance(questions[i], dict) else str(questions[i]),
                    "answer": "Error processing question",
                    "filename": "",
                    "page": 0
                })
            else:
                valid_answers.append(result)
        
        answers = valid_answers

        # Save output
        output_path = config.get("dataset", {}).get("output_path") or "./data/output.json"
        
        with open(output_path, "w") as f:
            json.dump(answers, f, indent=4, ensure_ascii=False)
        logger.info(f"Saved answers to {output_path}")

if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default="competition_config.yaml")
    parser.add_argument("--questions", default="./data/questions.json")
    parser.add_argument("--skip-ingest", action="store_true")
    parser.add_argument("--rebuild", action="store_true")
    args = parser.parse_args()
    
    asyncio.run(main_async(args))
