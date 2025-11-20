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

    # Ensure storages & parser ready
    await rag._ensure_lightrag_initialized()

    # Call aquery to get answer text (LightRAG returns str)
    answer_text = await rag.aquery(question, mode=mode)

    evidences: List[dict] = []
    if rag.lightrag and hasattr(rag.lightrag, "last_retrieved_chunks"):
        for chunk in rag.lightrag.last_retrieved_chunks or []:
            metadata = chunk.get("metadata", {})
            evidences.append(
                {
                    "filename": metadata.get("filename")
                    or metadata.get("source_filename"),
                    "page": metadata.get("page") or metadata.get("page_num"),
                    "content": chunk.get("content"),
                    "score": chunk.get("score"),
                }
            )

    return RetrievalResult(answer_text=answer_text, evidences=evidences)
