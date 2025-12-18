import argparse
import json
import os
import sys
from pathlib import Path


def _safe_int(v: object, default: int = 0) -> int:
    try:
        return int(v)  # type: ignore[arg-type]
    except Exception:
        return default


def _load_json(path: str) -> dict:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def _build_pdf_index(dataset_dir: str) -> dict:
    base = Path(dataset_dir)
    index: dict = {}
    if not base.exists():
        return index

    for p in base.rglob("*.pdf"):
        index[p.name] = str(p)

    return index


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--per-question",
        type=str,
        default="/root/rag/seekdb/rag/logs/per_question_eval.json",
    )
    parser.add_argument(
        "--dataset",
        type=str,
        default="/root/rag/seekdb/rag/data/dataset",
    )
    parser.add_argument("--min-answer-score", type=float, default=0.6)
    parser.add_argument("--run-vlm", action="store_true")
    parser.add_argument("--start", type=int, default=0)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--only-would-trigger", action="store_true")
    args = parser.parse_args()

    rag_dir = Path(__file__).resolve().parent
    if str(rag_dir) not in sys.path:
        sys.path.insert(0, str(rag_dir))

    vision_judge_page = None
    if args.run_vlm:
        from eval import vision_judge_page as _vision_judge_page

        vision_judge_page = _vision_judge_page

    data = _load_json(args.per_question)
    results = data.get("results") or []

    start = max(0, int(args.start or 0))
    end = len(results)
    if args.limit and args.limit > 0:
        end = min(end, start + int(args.limit))

    pdf_index = _build_pdf_index(args.dataset)

    total = 0
    should_trigger_cnt = 0
    pdf_found_cnt = 0
    would_trigger_cnt = 0
    eval_vision_used_cnt = 0

    for i in range(start, end):
        r = results[i] or {}
        qno = int(r.get("index", i)) + 1

        answer_score = float(r.get("answer_score") or 0.0)
        page_match = float(r.get("page_match") or 0.0)
        filename = str(r.get("player_filename") or "").strip()
        page = _safe_int(r.get("player_page"), 0)
        question = str(r.get("question") or "")

        should_trigger = (
            (answer_score > float(args.min_answer_score))
            and (page_match == 0.0)
            and (page > 0)
            and bool(filename)
        )

        expected_path = os.path.join(args.dataset, filename) if filename else ""
        expected_exists = bool(expected_path) and os.path.exists(expected_path)
        resolved_path = expected_path if expected_exists else pdf_index.get(Path(filename).name, "")
        resolved_exists = bool(resolved_path) and os.path.exists(resolved_path)

        would_trigger = should_trigger and resolved_exists

        vision_used = bool(r.get("vision_used"))
        vision_score = r.get("vision_score")

        if args.only_would_trigger and (not would_trigger):
            continue

        total += 1
        if should_trigger:
            should_trigger_cnt += 1
        if resolved_exists:
            pdf_found_cnt += 1
        if would_trigger:
            would_trigger_cnt += 1
        if vision_used:
            eval_vision_used_cnt += 1

        new_vision_score = None
        err = None
        if args.run_vlm and would_trigger and vision_judge_page is not None:
            try:
                new_vision_score = vision_judge_page(question, resolved_path, page)
            except Exception as e:
                err = f"{type(e).__name__}: {e}"

        print("=" * 120)
        print(f"Q{qno:02d} answer_score={answer_score:.4f} page_match={page_match:.1f} page={page}")
        print(f"should_trigger={should_trigger} pdf_exists={resolved_exists} would_trigger={would_trigger}")
        print(
            "eval_vision_used="
            + str(vision_used)
            + " eval_vision_score="
            + ("None" if vision_score is None else str(vision_score))
        )
        if filename:
            print("player_filename=", filename)
        if expected_path:
            print("expected_path=", expected_path)
        if resolved_path:
            print("resolved_path=", resolved_path)
        if new_vision_score is not None:
            print("run_vlm_score=", new_vision_score)
        if err is not None:
            print("run_vlm_error=", err)

    print("=" * 120)
    print(
        "summary: shown={} should_trigger={} pdf_found={} would_trigger={} eval_vision_used={}".format(
            total,
            should_trigger_cnt,
            pdf_found_cnt,
            would_trigger_cnt,
            eval_vision_used_cnt,
        )
    )


if __name__ == "__main__":
    main()
