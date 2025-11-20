"""Factories for constructing RAGAnything instances with docling configuration."""

from __future__ import annotations

import os
from typing import Callable, List

from lightrag.llm.openai import openai_complete_if_cache, openai_embed
from lightrag.utils import EmbeddingFunc

from src.anyrag_pipeline.raganything.raganything import RAGAnything
from src.config import PipelineConfig


DEFAULT_LLM_MODEL = "qwen-max"
DEFAULT_VLM_MODEL = "qwen-plus"
DEFAULT_EMBEDDING_MODEL = "text-embedding-v4"
DEFAULT_EMBEDDING_DIM = 1024


def _build_llm_model_func(api_key: str, base_url: str) -> Callable:
    model_name = os.getenv("LLM_MODEL_NAME", DEFAULT_LLM_MODEL)

    def llm_model_func(prompt: str, system_prompt=None, history_messages=None, **kwargs):
        history_messages = history_messages or []
        return openai_complete_if_cache(
            model_name,
            prompt,
            system_prompt=system_prompt,
            history_messages=history_messages,
            api_key=api_key,
            base_url=base_url,
            **kwargs,
        )

    return llm_model_func


def _build_vision_model_func(api_key: str, base_url: str) -> Callable:
    vision_model = os.getenv("VLM_MODEL_NAME", DEFAULT_VLM_MODEL)

    def vision_model_func(
        prompt: str,
        system_prompt=None,
        history_messages=None,
        image_data=None,
        messages=None,
        **kwargs,
    ):
        history_messages = history_messages or []
        if messages is not None:
            return openai_complete_if_cache(
                vision_model,
                "",
                system_prompt=None,
                history_messages=[],
                messages=messages,
                api_key=api_key,
                base_url=base_url,
                **kwargs,
            )
        return openai_complete_if_cache(
            vision_model,
            prompt,
            system_prompt=system_prompt,
            history_messages=history_messages,
            api_key=api_key,
            base_url=base_url,
            **kwargs,
        )

    return vision_model_func


def _build_embedding_func(api_key: str, base_url: str) -> EmbeddingFunc:
    embedding_model = os.getenv("EMBEDDING_MODEL", DEFAULT_EMBEDDING_MODEL)
    embedding_dim = int(os.getenv("EMBEDDING_DIM", str(DEFAULT_EMBEDDING_DIM)))
    max_token_size = int(os.getenv("EMBEDDING_MAX_TOKENS", "8192"))

    return EmbeddingFunc(
        embedding_dim=embedding_dim,
        max_token_size=max_token_size,
        func=lambda texts: openai_embed(
            texts,
            model=embedding_model,
            api_key=api_key,
            base_url=base_url,
        ),
    )


def build_raganything(pipeline_config: PipelineConfig) -> RAGAnything:
    api_key = os.getenv("OPENAI_API_KEY")
    base_url = os.getenv(
        "OPENAI_BASE_URL", "https://dashscope.aliyuncs.com/compatible-mode/v1"
    )
    if not api_key:
        raise RuntimeError("OPENAI_API_KEY is required to build the RAG pipeline")

    llm_model_func = _build_llm_model_func(api_key, base_url)
    vision_model_func = _build_vision_model_func(api_key, base_url)
    embedding_func = _build_embedding_func(api_key, base_url)

    rag = RAGAnything(
        config=pipeline_config.rag_config,
        llm_model_func=llm_model_func,
        vision_model_func=vision_model_func,
        embedding_func=embedding_func,
    )

    return rag
