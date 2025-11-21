"""Query helpers wrapping RAGAnything outputs for the CLI pipeline."""

from __future__ import annotations

from dataclasses import dataclass
from typing import List

from src.anyrag_pipeline.raganything.raganything import RAGAnything


@dataclass
class RetrievalResult:
    answer_text: str
    evidences: List[dict]


async def run_query(rag: RAGAnything, question: str, mode: str = "hybrid") -> RetrievalResult:
    """Run a question through RAGAnything and capture evidence metadata."""
    import re
    from collections import Counter
    from lightrag.base import QueryParam

    # Ensure storages & parser ready
    await rag._ensure_lightrag_initialized()

    # ========== 关键：使用 aquery_data 获取检索的chunks ==========
    # 先获取检索数据（不生成答案）
    query_param = QueryParam(mode=mode)
    result_data = await rag.lightrag.aquery_data(question, param=query_param)
    
    # 再生成答案
    answer_text = await rag.aquery(question, mode=mode)

    evidences: List[dict] = []
    
    # ========== 从检索结果中提取 file_path 元数据 ==========
    try:
        if result_data.get("status") == "success":
            data = result_data.get("data", {})
            chunks = data.get("chunks", [])
            
            # 解析每个chunk的file_path
            for chunk in chunks:
                file_path = chunk.get("file_path", "")
                content = chunk.get("content", "")
                
                # 解析 "filename#page=N" 格式
                if "#page=" in file_path:
                    parts = file_path.split("#page=")
                    filename = parts[0]
                    try:
                        page = int(parts[1])
                    except (ValueError, IndexError):
                        page = 1
                else:
                    # 如果没有页码信息，尝试从文件名提取
                    filename = file_path
                    page = 1
                
                if filename:
                    evidences.append({
                        "filename": filename,
                        "page": page,
                        "content": content,
                        "score": 1.0,
                    })
    
    except Exception as e:
        # 如果提取失败，记录错误
        import logging
        logging.warning(f"Failed to extract evidences from aquery_data: {e}")
    
    # 备用方案：从答案中的References部分提取filename和page
    if not evidences:
        # 改进策略：提取带编号的引用，优先选择[1]或出现最多的
        # 格式："- [1] 千味央厨-公司深度报告-22121364页.pdf"
        ref_pattern_with_num = r'-\s*\[(\d+)\]\s*(.+?)-(\d+)页\.pdf'
        ref_matches_numbered = re.findall(ref_pattern_with_num, answer_text)
        
        if ref_matches_numbered:
            # ref_matches_numbered = [(ref_num, filename_prefix, page_num), ...]
            # 策略1：优先选择[1]号引用（通常是最相关的）
            for ref_num, filename_prefix, page_num in ref_matches_numbered:
                if ref_num == "1":
                    full_filename = f"{filename_prefix}-{page_num}页.pdf"
                    evidences.append({
                        "filename": full_filename.strip(),
                        "page": int(page_num),
                        "content": "",
                        "score": 0.9,  # [1]号引用，高置信度
                    })
                    break
            
            # 策略2：如果没有[1]，统计出现最多的
            if not evidences:
                evidence_counter = Counter()
                for ref_num, filename_prefix, page_num in ref_matches_numbered:
                    full_filename = f"{filename_prefix}-{page_num}页.pdf"
                    evidence_counter[(full_filename, int(page_num))] += 1
                
                if evidence_counter:
                    most_common = evidence_counter.most_common(1)[0][0]
                    evidences.append({
                        "filename": most_common[0].strip(),
                        "page": most_common[1],
                        "content": "",
                        "score": 0.7,  # 统计选择，中等置信度
                    })
        
        # 策略3：如果没有编号引用，尝试提取第一个文件名
        if not evidences:
            ref_pattern_simple = r'-\s*\[?\d*\]?\s*([^-\n]+?-\d+页\.pdf)'
            ref_matches_simple = re.findall(ref_pattern_simple, answer_text)
            if ref_matches_simple:
                # 取第一个作为最相关
                first_ref = ref_matches_simple[0]
                evidences.append({
                    "filename": first_ref.strip(),
                    "page": 1,  # 无法准确提取页码
                    "content": "",
                    "score": 0.3,  # 低置信度
                })
        else:
            # 方法2：只提取文件名（无页码）
            ref_pattern = r'-\s*\[?\d+\]?\s*([^-\n]+\.pdf)'
            ref_matches = re.findall(ref_pattern, answer_text)
            
            if ref_matches:
                filename_counter = Counter(ref_matches)
                most_common_file = filename_counter.most_common(1)[0][0] if filename_counter else ""
                
                if most_common_file:
                    evidences.append({
                        "filename": most_common_file.strip(),
                        "page": 1,  # 无法确定页码，默认第1页
                        "content": "",
                        "score": 0.3,  # 低置信度
                    })
    
    return RetrievalResult(answer_text=answer_text, evidences=evidences)
