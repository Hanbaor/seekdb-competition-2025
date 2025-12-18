import argparse
import json
import logging
import os
import sys
import time

# 根据你的路径调整
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(__file__)))  # /root/rag/seekdb/rag 的父目录
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

# 设置正确的RAG目录
RAG_DIR = "/root/rag/seekdb/rag"  # 直接指定为你的实际目录
if RAG_DIR not in sys.path:
    sys.path.insert(0, RAG_DIR)

from eval import llm_judge, vision_judge_page
from src.util import Answer, read_train_json


def _safe_int(v: object, default: int = 0) -> int:
    try:
        return int(v)  # type: ignore[arg-type]
    except Exception:
        return default


def evaluate_single(
    dataset_dir: str,
    player_answer: Answer,
    normal_answer: Answer,
    *,
    disable_vlm: bool,
) -> dict:
    start = time.time()

    question = player_answer.question or normal_answer.question or ""

    answer_score = llm_judge(normal_answer.answer or "", player_answer.answer or "")

    judge_failed = False
    if answer_score is None:
        judge_failed = True
        answer_score = 0.0

    filename_score = 0.0
    page_score = 0.0
    file_match_score = 0.0

    if (player_answer.filename or "") and player_answer.filename == (normal_answer.filename or ""):
        filename_score = 1.0
        if _safe_int(player_answer.page) == _safe_int(normal_answer.page):
            page_score = 1.0
        file_match_score = (filename_score + page_score) / 2.0

    vision_used = False
    vision_score = None

    page_num = _safe_int(player_answer.page)
    if (
        (not disable_vlm)
        and answer_score > 0.6
        and page_score == 0.0
        and page_num > 0
        and (player_answer.filename or "")
    ):
        pdf_path = os.path.join(dataset_dir, player_answer.filename)
        if os.path.exists(pdf_path):
            try:
                vision_score = vision_judge_page(question, pdf_path, page_num)
            except Exception:
                vision_score = None

            if vision_score is not None:
                vision_used = True
                file_match_score = float(vision_score)

    final_score = file_match_score * 30.0 + float(answer_score) * 70.0

    elapsed_sec = time.time() - start

    return {
        "question": question,
        "answer_score": float(answer_score),
        "judge_failed": judge_failed,
        "file_match_score": float(file_match_score),
        "filename_match": float(filename_score),
        "page_match": float(page_score),
        "vision_used": vision_used,
        "vision_score": None if vision_score is None else float(vision_score),
        "final_score": float(final_score),
        "player_filename": player_answer.filename,
        "player_page": _safe_int(player_answer.page),
        "normal_filename": normal_answer.filename,
        "normal_page": _safe_int(normal_answer.page),
        "player_answer": (player_answer.answer or "")[:200],
        "normal_answer": (normal_answer.answer or "")[:200],
        "elapsed_sec": float(elapsed_sec),
    }


def parse_args() -> argparse.Namespace:
    # 直接使用你的实际数据目录
    data_dir = "/root/rag/seekdb/rag/data"
    default_dataset_dir = os.path.join(data_dir, "dataset")
    
    # 检查目录是否存在，不存在则创建
    for dir_path in [data_dir, default_dataset_dir]:
        if not os.path.exists(dir_path):
            os.makedirs(dir_path, exist_ok=True)
            logging.info(f"Created directory: {dir_path}")

    parser = argparse.ArgumentParser(description="Per-question evaluation")
    parser.add_argument("--dataset", type=str, default=default_dataset_dir,
                       help=f"Dataset directory (default: {default_dataset_dir})")
    parser.add_argument("--output", type=str, 
                       default="/root/rag/seekdb/rag/data/output.json",
                       help="Path to player output JSON file")
    parser.add_argument("--answer", type=str,
                       default="/root/rag/seekdb/rag/data/answer.json",
                       help="Path to ground truth answer JSON file")
    parser.add_argument(
        "--out",
        type=str,
        default=os.path.join(RAG_DIR, "logs", "per_question_eval.json"),
        help="Path to output evaluation results"
    )
    parser.add_argument("--start", type=int, default=0,
                       help="Start index (0-based)")
    parser.add_argument("--limit", type=int, default=0,
                       help="Limit number of questions to evaluate (0 for all)")
    parser.add_argument("--disable-vlm", action="store_true",
                       help="Disable VLM vision judgment")
    parser.add_argument("--sort", choices=["index", "final_score"], default="index",
                       help="Sort results by index or final_score")

    return parser.parse_args()


def main() -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s - %(name)s - %(levelname)s - %(message)s",
    )
    logger = logging.getLogger("eval_per_question")

    args = parse_args()
    
    # 打印路径信息用于调试
    logger.info("Working directory: %s", os.getcwd())
    logger.info("RAG_DIR: %s", RAG_DIR)
    logger.info("Output file path: %s", args.output)
    logger.info("Answer file path: %s", args.answer)
    logger.info("Dataset dir: %s", args.dataset)
    
    # 检查文件是否存在
    if not os.path.exists(args.output):
        logger.error("Output file not found: %s", args.output)
        logger.info("Please generate output.json first or specify correct path with --output")
        sys.exit(1)
    
    if not os.path.exists(args.answer):
        logger.error("Answer file not found: %s", args.answer)
        logger.info("Please ensure answer.json exists in data directory")
        sys.exit(1)

    try:
        player_answers = read_train_json(args.output)
        normal_answers = read_train_json(args.answer)
    except Exception as e:
        logger.error("Failed to read JSON files: %s", e)
        sys.exit(1)

    total = min(len(player_answers), len(normal_answers))
    start = max(0, int(args.start or 0))
    end = total
    if args.limit and args.limit > 0:
        end = min(total, start + int(args.limit))
    
    logger.info("Evaluating %d questions (start=%d, end=%d)", end-start, start, end)

    results = []
    scores = []
    answer_scores = []
    file_scores = []

    for i in range(start, end):
        player = player_answers[i]
        normal = normal_answers[i]
        if not player.question:
            player.question = normal.question

        logger.info("[%d/%d] Evaluating: %s", i + 1, end, (player.question or "")[:80])

        try:
            r = evaluate_single(args.dataset, player, normal, disable_vlm=bool(args.disable_vlm))
            r["index"] = i
            results.append(r)
            scores.append(r["final_score"])
            answer_scores.append(r["answer_score"])
            file_scores.append(r["file_match_score"])

            logger.info(
                "  final=%.4f (answer=%.4f, file=%.4f) judge_failed=%s",
                r["final_score"],
                r["answer_score"],
                r["file_match_score"],
                r["judge_failed"],
            )
        except Exception as e:
            logger.error("  evaluation failed: %s", e, exc_info=True)
            results.append(
                {
                    "index": i,
                    "question": player.question or normal.question or "",
                    "error": str(e),
                }
            )

    if args.sort == "final_score":
        results.sort(key=lambda x: (x.get("final_score") is None, x.get("final_score", 0.0)))

    # 计算统计数据
    if scores:
        summary = {
            "num_items": len(results),
            "avg_final_score": sum(scores) / len(scores),
            "avg_answer_score": sum(answer_scores) / len(answer_scores),
            "avg_file_match_score": sum(file_scores) / len(file_scores),
            "results": results,
        }
    else:
        summary = {
            "num_items": 0,
            "avg_final_score": 0.0,
            "avg_answer_score": 0.0,
            "avg_file_match_score": 0.0,
            "results": results,
        }

    # 确保输出目录存在
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(summary, f, ensure_ascii=False, indent=2)

    logger.info("Saved per-question results to: %s", args.out)
    if scores:
        logger.info("Avg final score: %.4f", summary["avg_final_score"])
        logger.info("Avg answer score: %.4f", summary["avg_answer_score"])
        logger.info("Avg file match score: %.4f", summary["avg_file_match_score"])


if __name__ == "__main__":
    main()