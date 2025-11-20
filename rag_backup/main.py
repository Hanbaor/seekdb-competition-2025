import argparse
import asyncio
import logging
import os
import sys
from pathlib import Path
import shutil
from typing import List

PROJECT_ROOT = Path(__file__).resolve().parent


def configure_hf_cache() -> None:
    local_hf_home = PROJECT_ROOT / "hf_cache"
    global_hf_home = Path.home() / ".cache" / "huggingface" / "hub"
    local_models_root = local_hf_home / "hub"

    # Ensure required Docling models are available in the project-local cache
    for models_dir_name in [
        "models--docling-project--docling-layout-heron",
        "models--docling-project--docling-models",
    ]:
        local_models_dir = local_models_root / models_dir_name
        if not local_models_dir.exists():
            global_models_dir = global_hf_home / models_dir_name
            if global_models_dir.exists():
                local_models_root.mkdir(parents=True, exist_ok=True)
                shutil.copytree(global_models_dir, local_models_dir)

    local_hf_home.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("HF_HOME", str(local_hf_home))
    os.environ.setdefault("HF_HUB_OFFLINE", "1")


def delete_global_docling_cache() -> None:
    global_models_dir = (
        Path.home()
        / ".cache"
        / "huggingface"
        / "hub"
        / "models--docling-project--docling-layout-heron"
    )
    if global_models_dir.exists():
        shutil.rmtree(global_models_dir)


configure_hf_cache()

SRC_PATH = PROJECT_ROOT / "src"
if str(SRC_PATH) not in sys.path:
    sys.path.insert(0, str(SRC_PATH))

from src.anyrag_pipeline.answer_formatter import format_answer
from src.anyrag_pipeline.builder import build_raganything
from src.anyrag_pipeline.document_loader import process_dataset
from src.anyrag_pipeline.query_engine import run_query
from src.config import load_pipeline_config
from src.util.util import read_questions_json, write_answers

logger = logging.getLogger(__name__)


def parse_args():
    """
    Parse command line arguments.

    Returns:
        argparse.Namespace: Parsed arguments containing:
            - dataset: Path to dataset directory
            - questions: Path to questions/test JSON file
            - output: Path to output JSON file
    """
    parser = argparse.ArgumentParser(
        description="RAG system for question answering",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
    python3.10 main.py --dataset="./data/dataset/" \\
                       --questions="./data/questions.json" \\
                       --output="./data/output.json"
        """,
    )

    parser.add_argument(
        "--dataset",
        type=str,
        default="./data/dataset/",
        help="Path to dataset directory (default: ./data/dataset/)",
    )

    parser.add_argument(
        "--questions",
        type=str,
        default="./data/questions.json",
        help="Path to questions/test JSON file (default: ./data/questions.json)",
    )

    parser.add_argument(
        "--output",
        type=str,
        default="./data/output.json",
        help="Path to output JSON file (default: ./data/output.json)",
    )
    parser.add_argument(
        "--rebuild-index",
        action="store_true",
        help="Force rebuild of document cache and index",
    )
    parser.add_argument(
        "--cleanup-hf-global-cache",
        action="store_true",
        help="Delete global HuggingFace docling-layout-heron cache and exit",
    )

    args = parser.parse_args()

    # Validate paths
    if not os.path.isdir(args.dataset):
        raise ValueError(f"Dataset directory does not exist: {args.dataset}")

    if not os.path.isfile(args.questions):
        raise ValueError(f"Questions file does not exist: {args.questions}")

    # Create output directory if it doesn't exist
    output_dir = os.path.dirname(args.output)
    if output_dir and not os.path.exists(output_dir):
        os.makedirs(output_dir, exist_ok=True)

    return args


async def main_async(args):
    pipeline_config = load_pipeline_config(rebuild_index=args.rebuild_index)
    rag = build_raganything(pipeline_config)

    await process_dataset(
        rag,
        dataset_dir=args.dataset,
        rebuild=args.rebuild_index,
        parse_method=pipeline_config.rag_config.parse_method,
    )

    questions: List[str] = read_questions_json(args.questions)
    answers = []
    for idx, question in enumerate(questions, 1):
        logging.info(f"Querying question {idx}/{len(questions)}")
        retrieval = await run_query(rag, question, mode="hybrid")
        answers.append(format_answer(question, retrieval))

    write_answers(answers, args.output)


if __name__ == "__main__":
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s - %(name)s - %(levelname)s - %(message)s",
    )

    args = parse_args()

    logger.info(f"Dataset directory: {args.dataset}")
    logger.info(f"Questions file: {args.questions}")
    logger.info(f"Output file: {args.output}")

    if args.cleanup_hf_global_cache:
        delete_global_docling_cache()
        logger.info("Deleted global HuggingFace cache for docling-layout-heron.")
        sys.exit(0)

    try:
        asyncio.run(main_async(args))
        logger.info("All tasks completed successfully.")
    except Exception as exc:
        logger.error(f"Pipeline execution failed: {exc}", exc_info=True)
        raise
