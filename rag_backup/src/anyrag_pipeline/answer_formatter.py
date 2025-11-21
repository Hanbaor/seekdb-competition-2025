"""Utilities to convert retrieval results into Answer objects."""

from __future__ import annotations

from typing import Optional

from src.util.util import Answer
from src.anyrag_pipeline.query_engine import RetrievalResult


def pick_best_evidence(result: RetrievalResult) -> Optional[dict]:
    """
    从多个evidences中选择最佳的filename和page。
    
    策略（按优先级）：
    1. 投票机制：选择出现次数最多的(filename, page)组合
    2. 如果投票平局，选择score最高的
    3. 如果还是无法决定，返回第一个
    """
    if not result.evidences:
        return None
    
    from collections import Counter
    
    # 过滤掉无效的evidences
    valid_evidences = [
        e for e in result.evidences
        if e.get("filename") and e.get("page")
    ]
    
    if not valid_evidences:
        return None
    
    # 策略1：投票 - 选择出现最多的(filename, page)组合
    evidence_pairs = [
        (e.get("filename"), e.get("page")) 
        for e in valid_evidences
    ]
    pair_counter = Counter(evidence_pairs)
    
    # 如果有明显的多数票
    most_common_pairs = pair_counter.most_common(2)
    if len(most_common_pairs) == 1 or most_common_pairs[0][1] > most_common_pairs[1][1]:
        # 返回出现最多的那个evidence
        best_pair = most_common_pairs[0][0]
        for e in valid_evidences:
            if e.get("filename") == best_pair[0] and e.get("page") == best_pair[1]:
                return e
    
    # 策略2：如果平局，按score排序
    evidences_sorted = sorted(
        valid_evidences,
        key=lambda e: e.get("score", 0),
        reverse=True,
    )
    return evidences_sorted[0]


def format_answer(question: str, retrieval: RetrievalResult) -> Answer:
    evidence = pick_best_evidence(retrieval)
    filename = evidence.get("filename") if evidence else ""
    page = int(evidence.get("page")) if evidence and evidence.get("page") else 0

    # 始终使用LightRAG生成的答案
    answer_text = retrieval.answer_text if retrieval.answer_text else "无法从提供的资料中确定答案。"

    return Answer(question=question, answer=answer_text, filename=filename, page=page)
