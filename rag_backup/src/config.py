"""Configuration helpers for the docling-based RAGAnything pipeline."""

from __future__ import annotations

import os
from dataclasses import dataclass

from src.anyrag_pipeline.raganything.config import RAGAnythingConfig


def _env_bool(key: str, default: bool) -> bool:
    """Fetch boolean environment variables with sensible defaults."""

    value = os.getenv(key)
    if value is None:
        return default
    return value.strip().lower() in {"true", "1", "yes", "y", "on"}


@dataclass
class PipelineConfig:
    """Top-level configuration used by the CLI pipeline."""

    rag_config: RAGAnythingConfig
    working_dir: str
    rebuild_index: bool = False


def load_pipeline_config(rebuild_index: bool = False) -> PipelineConfig:
    """Load configuration from environment variables.

    Args:
        rebuild_index: Whether the current run should rebuild the index/cache
            regardless of cached artifacts.
    """

    working_dir = os.getenv("OUTPUT_DIR", os.getenv("WORKING_DIR", "./rag_storage"))

    rag_config = RAGAnythingConfig(
        working_dir=working_dir,
        parser=os.getenv("PARSER", "docling"),
        parse_method=os.getenv("PARSE_METHOD", "auto"),
        enable_image_processing=_env_bool("ENABLE_IMAGE_PROCESSING", True),
        enable_table_processing=_env_bool("ENABLE_TABLE_PROCESSING", True),
        enable_equation_processing=_env_bool("ENABLE_EQUATION_PROCESSING", True),
    )

    return PipelineConfig(rag_config=rag_config, working_dir=working_dir, rebuild_index=rebuild_index)
