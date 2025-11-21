#!/usr/bin/env python3
"""
简化版RAG：只用FAISS向量检索，跳过图和树构建
对比测试E2GraphRAG是否真的有用
"""
import asyncio
import argparse
import yaml
import json
import os
import re
from pathlib import Path
from typing import List, Dict
import logging

from dotenv import load_dotenv
from api_client import APIClient
from simple_vector_retriever import SimpleVectorRetriever

# 加载环境变量
load_dotenv()

logging.basicConfig(
    level=logging.INFO,
    format='%(levelname)s:%(name)s:%(message)s'
)
logger = logging.getLogger(__name__)


def extract_source_info(chunks_text: str, doc_name: str) -> tuple:
    """从chunks中提取文件名和页码"""
    # 提取页码
    page_pattern = r'\[PAGE_START:(\d+)\]'
    pages = re.findall(page_pattern, chunks_text)
    page = int(pages[0]) if pages else 0
    
    # 文件名：从doc_name中提取
    filename = doc_name.replace('页', '页.pdf')
    if not filename.endswith('.pdf'):
        filename += '.pdf'
    
    return filename, page


async def main_async(args):
    """主函数"""
    # 加载配置
    with open(args.config, 'r', encoding='utf-8') as f:
        config = yaml.safe_load(f)
    
    # 初始化API客户端
    api_client = APIClient(config)
    
    # 加载问题
    with open(args.questions, 'r', encoding='utf-8') as f:
        questions = json.load(f)
    
    # 数据路径 - 从E2GraphRAG的cache读取
    cache_dir = Path(config["paths"]["cache_path"]) / config["dataset"]["dataset_name"]
    index_dir = Path("simple_indexes")
    index_dir.mkdir(exist_ok=True)
    
    # 获取所有文档（从cache中的子目录）
    doc_dirs = [d for d in cache_dir.iterdir() if d.is_dir()]
    doc_names = [d.name for d in doc_dirs]
    
    logger.info(f"Found {len(doc_names)} documents in {cache_dir}")
    
    # 构建或加载索引
    retrievers = []
    
    if not args.skip_ingest:
        logger.info("Building simple vector indexes...")
        
        # 初始化embedding模型
        from api_embedding import APIEmbedding
        embedder = APIEmbedding(
            model_name=config['retriever']['kwargs']['embedding_model'],
            embedding_dim=config['retriever']['kwargs']['embedding_dim']
        )
        
        for doc_dir in doc_dirs:
            doc_name = doc_dir.name
            index_path = index_dir / doc_name
            
            # 检查是否已存在
            if (index_path.with_suffix('.pkl').exists() and 
                index_path.with_suffix('.faiss').exists()):
                logger.info(f"Loading existing index for {doc_name}")
                retriever = SimpleVectorRetriever.load(str(index_path), embedder)
            else:
                logger.info(f"Building index for {doc_name}")
                
                # 读取VLM解析的文本（从vlm_results.json）
                vlm_results_file = doc_dir / "vlm_results.json"
                if not vlm_results_file.exists():
                    logger.warning(f"No vlm_results.json for {doc_name}, skipping")
                    continue
                
                with open(vlm_results_file, 'r', encoding='utf-8') as f:
                    vlm_results = json.load(f)
                
                # 拼接所有page的内容（提取text字段）
                texts = []
                for page_data in vlm_results.values():
                    if isinstance(page_data, dict) and 'text' in page_data:
                        texts.append(page_data['text'])
                    elif isinstance(page_data, str):
                        texts.append(page_data)
                
                vlm_text = "\n\n".join(texts)
                
                # 创建索引
                chunk_size = config.get('cluster', {}).get('chunk_length', 512)
                retriever = SimpleVectorRetriever.from_vlm_output(
                    doc_name, vlm_text, embedder, 
                    chunk_size=chunk_size
                )
                
                # 保存索引
                retriever.save(str(index_path))
            
            retrievers.append(retriever)
        
        logger.info(f"Loaded {len(retrievers)} retrievers")
    else:
        logger.info("Loading existing indexes...")
        
        from api_embedding import APIEmbedding
        embedder = APIEmbedding(
            model_name=config['retriever']['kwargs']['embedding_model'],
            embedding_dim=config['retriever']['kwargs']['embedding_dim']
        )
        
        for doc_name in doc_names:
            index_path = index_dir / doc_name
            if (index_path.with_suffix('.pkl').exists() and 
                index_path.with_suffix('.faiss').exists()):
                retriever = SimpleVectorRetriever.load(str(index_path), embedder)
                retrievers.append(retriever)
                logger.info(f"Loaded retriever for {doc_name}")
            else:
                logger.warning(f"Index not found for {doc_name}, skipping")
    
    # 查询
    logger.info(f"Starting query for {len(questions)} questions")
    
    # 创建debug输出目录
    debug_dir = Path("debug_output_simple")
    debug_dir.mkdir(exist_ok=True)
    
    results = []
    
    for idx, question_item in enumerate(questions, 1):
        if isinstance(question_item, dict):
            question = question_item.get("question", "")
        else:
            question = str(question_item)
        
        logger.info(f"[{idx}/{len(questions)}] Processing: {question}")
        
        # 预计算query embedding，避免重复调用API
        import faiss
        import numpy as np
        query_emb = embedder.encode(question).reshape(1, -1).astype('float32')
        faiss.normalize_L2(query_emb)
        
        # 从所有文档检索
        all_retrievals = []
        best_score = -1
        best_chunks = ""
        best_doc = ""
        
        for retriever in retrievers:
            # 传入预计算的embedding
            result = retriever.query(question, top_k=6, query_emb=query_emb)
            
            # 记录检索信息
            retrieval_info = {
                "doc_name": retriever.doc_name,
                "retrieval_type": result["retrieval_type"],
                "chunks_text": result["chunks"],
                "chunks_length": len(result["chunks"]),
                "scores": result.get("scores", []),
                "avg_score": sum(result.get("scores", [0])) / max(len(result.get("scores", [1])), 1)
            }
            all_retrievals.append(retrieval_info)
            
            # 选择平均得分最高的文档
            avg_score = retrieval_info["avg_score"]
            if avg_score > best_score:
                best_score = avg_score
                best_chunks = result["chunks"]
                best_doc = retriever.doc_name
        
        # 生成答案
        prompt = f"""Based on the following context, answer the question.
If the answer is not in the context, say "I don't know".

Context:
{best_chunks}

Question: {question}
Answer:"""
        
        messages = [{"role": "user", "content": prompt}]
        answer_text = await api_client.call_vlm(messages)
        
        # 提取源信息
        filename, page = extract_source_info(best_chunks, best_doc)
        
        # 保存debug信息
        debug_info = {
            "question": question,
            "retrievals": all_retrievals,
            "best_doc": best_doc,
            "best_score": best_score,
            "source": {"filename": filename, "page": page},
            "answer": answer_text
        }
        
        debug_file = debug_dir / f"question_{idx:02d}.json"
        with open(debug_file, 'w', encoding='utf-8') as f:
            json.dump(debug_info, f, indent=2, ensure_ascii=False)
        
        results.append({
            "question": question,
            "answer": answer_text,
            "filename": filename,
            "page": page
        })
        
        logger.info(f"[{idx}/{len(questions)}] Completed")
    
    # 保存结果
    output_file = Path(args.output) if args.output else Path("data/output_simple.json")
    output_file.parent.mkdir(exist_ok=True, parents=True)
    
    with open(output_file, 'w', encoding='utf-8') as f:
        json.dump(results, f, indent=2, ensure_ascii=False)
    
    logger.info(f"Saved answers to {output_file}")


def main():
    parser = argparse.ArgumentParser(description="Simple Vector RAG (skip graph & tree)")
    parser.add_argument("--config", required=True, help="Path to config YAML")
    parser.add_argument("--questions", required=True, help="Path to questions JSON")
    parser.add_argument("--skip-ingest", action="store_true", help="Skip ingestion, use existing indexes")
    parser.add_argument("--output", help="Output JSON path (default: data/output_simple.json)")
    
    args = parser.parse_args()
    
    asyncio.run(main_async(args))


if __name__ == "__main__":
    main()
