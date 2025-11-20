"""Utilities to convert retrieval results into Answer objects."""

from __future__ import annotations

from typing import Optional

from src.util.util import Answer
from src.anyrag_pipeline.query_engine import RetrievalResult


def pick_best_evidence(result: RetrievalResult) -> Optional[dict]:
    if not result.evidences:
        return None
    # Simple heuristic: prefer evidence with filename+page, else fallback first
    evidences = sorted(
        result.evidences,
        key=lambda e: (e.get("filename") is not None, e.get("page") is not None, e.get("score") or 0),
        reverse=True,
    )
    return evidences[0]


def format_answer(question: str, retrieval: RetrievalResult) -> Answer:
    evidence = pick_best_evidence(retrieval)
    filename = evidence.get("filename") if evidence else ""
    page = int(evidence.get("page")) if evidence and evidence.get("page") else 0

    if not filename:
        answer_text = "无法从提供的资料中确定答案。"
    else:
        answer_text = retrieval.answer_text

    return Answer(question=question, answer=answer_text, filename=filename, page=page)
