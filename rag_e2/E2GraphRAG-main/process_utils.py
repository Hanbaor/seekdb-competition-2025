import os
import logging
from dataclasses import dataclass

import torch
from openai import OpenAI

from build_tree import build_tree, load_cache_summary
from extract_graph import extract_graph, load_nlp, load_cache

def clean_cuda_memory(device_id):
    """清理指定GPU设备的缓存"""
    if torch.cuda.is_available():
        with torch.cuda.device(device_id):
            torch.cuda.empty_cache()

logger = logging.getLogger(__name__)


@dataclass
class APIPipelineConfig:
    model_name: str
    temperature: float = 0.01
    max_tokens: int = 2048


class ChatCompletionPipeline:
    """Mimic transformers.pipeline interface using OpenAI-compatible REST API."""

    def __init__(self, config: APIPipelineConfig):
        api_key = os.getenv("OPENAI_API_KEY")
        base_url = os.getenv("OPENAI_BASE_URL")
        if not api_key:
            raise RuntimeError("OPENAI_API_KEY is required for API-based tree building.")
        if not base_url:
            raise RuntimeError("OPENAI_BASE_URL is required for API-based tree building.")

        self.client = OpenAI(api_key=api_key, base_url=base_url)
        self.config = config

    def __call__(self, prompt: str):
        if not isinstance(prompt, str):
            raise ValueError("ChatCompletionPipeline only supports single-string prompts.")

        logger.debug("[TreeBuild][API] Sending prompt with %d chars", len(prompt))
        response = self.client.chat.completions.create(
            model=self.config.model_name,
            messages=[{"role": "user", "content": prompt}],
            temperature=self.config.temperature,
            # max_tokens=self.config.max_tokens,
        )
        logger.debug("[TreeBuild][API] Received completion choices=%d", len(response.choices))
        completion = response.choices[0].message.content or ""
        # Keep compatibility with transformers pipeline output format
        return [{"generated_text": f"{prompt}{completion}"}]


def build_tree_task(args):
    llm_path, llm_device, text, cache_folder, tokenizer_name, length, overlap, merge_num, torch_dtype, language = args
    if os.path.exists(os.path.join(cache_folder, "tree.json")):
        return load_cache_summary(os.path.join(cache_folder, "tree.json")), -1
    try:
        device_id = int(llm_device.split(':')[1]) if ':' in llm_device else 0
        tokenizer = None
        logger.info("[TreeBuild] Starting tree construction via API model '%s'", llm_path)
        llm_pipeline = ChatCompletionPipeline(
            APIPipelineConfig(model_name=llm_path)
        )
        
        # Process
        logger.info("[TreeBuild] Building tree cache at %s", cache_folder)
        result, time_cost = build_tree(text, llm_pipeline, cache_folder, tokenizer, length, overlap, merge_num, language)
        print(f"build tree task result type: {type(result)}")
        print(f"build tree task time cost: {time_cost}, -1 means load from cache.")
        logger.info("[TreeBuild] Tree building finished in %.2fs", time_cost if time_cost != -1 else 0.0)
        return result, time_cost
    except Exception as e:
        print(f"build tree task error: {e}")
        print(f"{type(e).__name__}")
        print(f"{e.args}")
        import traceback
        print(f"build tree task error stack:\n{traceback.format_exc()}")
        raise e
    finally:
        # Clean up
        if 'llm_pipeline' in locals() and llm_pipeline is not None:
            del llm_pipeline
        clean_cuda_memory(device_id)

def extract_graph_task(args):
    text, cache_folder, language = args
    if os.path.exists(os.path.join(cache_folder, "graph.json")):
        return load_cache(cache_folder), -1
    try:
        # Load NLP model in subprocess
        logger.info("[GraphExtract] Loading spaCy model for language=%s", language)
        nlp = load_nlp(language)
        logger.info("[GraphExtract] Starting graph extraction for %s", cache_folder)
        (result, index, count), time_cost = extract_graph(text, cache_folder, nlp)
        logger.info("[GraphExtract] spaCy extraction finished in %.2fs", time_cost if time_cost != -1 else 0.0)
        print(f"extract graph task result type: {type(result)}")
        print(f"extract graph task time cost: {time_cost}, -1 means load from cache.")
        return (result, index, count), time_cost
    finally:
        # Clean up
        del nlp 