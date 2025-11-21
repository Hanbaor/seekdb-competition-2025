import argparse
import logging
import os
# Configure no_proxy to bypass system proxy for local SSH tunnel
os.environ["NO_PROXY"] = os.environ.get("NO_PROXY", "") + ",localhost,127.0.0.1"

from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime
from typing import List

from src.core import add, search
from src.util import Answer, read_questions_json, write_answers

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


if __name__ == "__main__":
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s - %(name)s - %(levelname)s - %(message)s",
    )

    args = parse_args()

    logger.info(f"Dataset directory: {args.dataset}")
    logger.info(f"Questions file: {args.questions}")
    logger.info(f"Output file: {args.output}")

    logger.info("Starting data preparation...")
    add(
        dataset_dir=args.dataset,
        max_worker=10,
    )
    logger.info("Data preparation completed.")

    logger.info("Starting question answering...")
    questions: List[str] = read_questions_json(args.questions)
    logger.info(f"Loaded {len(questions)} questions from {args.questions}")

    # Process questions in parallel with max 10 workers
    max_qa_workers = 10
    answers_dict = {}  # Store answers with their original index
    
    with ThreadPoolExecutor(max_workers=max_qa_workers) as executor:
        # Submit all question processing tasks
        future_to_idx = {
            executor.submit(search, question): idx
            for idx, question in enumerate(questions)
        }
        
        # Collect results as they complete
        for future in as_completed(future_to_idx):
            idx = future_to_idx[future]
            question = questions[idx]
            try:
                answer = future.result()
                answers_dict[idx] = answer
                logger.info(f"Completed question {idx + 1}/{len(questions)}: {question[:50]}...")
            except Exception as e:
                logger.error(f"Error processing question {idx + 1}: {e}", exc_info=True)
                # Create empty answer on error
                answers_dict[idx] = Answer(question=question, answer="", contexts=[])
    
    # Sort answers by index to maintain original order
    answers: List[Answer] = [answers_dict[i] for i in range(len(questions))]
    
    logger.info(f"Completed processing {len(answers)} questions.")
    logger.info(f"Writing answers to {args.output}...")
    write_answers(answers, args.output)
    logger.info("All tasks completed successfully.")
