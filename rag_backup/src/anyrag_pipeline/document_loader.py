"""Document processing helpers for the RAGAnything pipeline."""

from __future__ import annotations

import asyncio
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
    """Process every PDF in the dataset directory using RAGAnything."""

    dataset_path = Path(dataset_dir)
    if not dataset_path.exists():
        raise FileNotFoundError(f"Dataset directory not found: {dataset_dir}")

    pdf_files = sorted(p for p in dataset_path.glob("*.pdf"))
    if not pdf_files:
        logger.warning("No PDF files found in dataset directory")
        return

    processed_dir = Path(rag.working_dir) / "processed_documents"
    processed_dir.mkdir(parents=True, exist_ok=True)

    logger.info(f"Processing {len(pdf_files)} PDFs from {dataset_dir}")

    for pdf in pdf_files:
        marker = processed_dir / f"{pdf.stem}.done"
        if not rebuild and marker.exists():
            logger.info(f"Skipping cached document: {pdf.name}")
            continue

        logger.info(f"Processing document: {pdf.name}")
        await rag.process_document_complete(
            file_path=str(pdf),
            output_dir=rag.working_dir,
            parse_method=parse_method or rag.config.parse_method,
        )

        marker.touch()
