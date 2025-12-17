import logging
import os
import time
import json
import re
import hashlib

import dotenv

from src.integrations.embedding import generate_response as generate_embedding
from src.integrations.llm import generate_response as generate_llm_response
from src.prompt import QUERY_SYSTEM_PROMPT, QUERY_USER_PROMPT_TEMPLATE
from src.storage.oceanbase import get_or_create_client
from src.util import Answer

logger = logging.getLogger(__name__)

dotenv.load_dotenv()

# OceanBase connection parameters
OCEANBASE_URI = os.getenv("OCEANBASE_URI")
OCEANBASE_USER = os.getenv("OCEANBASE_USER")
OCEANBASE_PASSWORD = os.getenv("OCEANBASE_PASSWORD")
OCEANBASE_DBNAME = os.getenv("OCEANBASE_DBNAME")

# Table name for storing document chunks
TABLE_NAME = "rag_documents"

# Number of top results to retrieve
# TOP_K = 30  # Increased to 30 for better coverage across 20 PDFs
TOP_K = 50  # Increased for better recall

# Enable LLM Rerank: Use LLM to rerank top results and select best file/page
USE_LLM_RERANK = True  # Set to True to enable LLM reranking


def _env_bool(name: str, default: bool = False) -> bool:
    v = os.getenv(name)
    if v is None:
        return default
    return str(v).strip().lower() in {"1", "true", "yes", "y", "on"}


def _env_int(name: str, default: int) -> int:
    v = os.getenv(name)
    if v is None or str(v).strip() == "":
        return default
    try:
        return int(str(v).strip())
    except Exception:
        return default


def _env_float(name: str, default: float) -> float:
    v = os.getenv(name)
    if v is None or str(v).strip() == "":
        return default
    try:
        return float(str(v).strip())
    except Exception:
        return default


USE_BATCH_SUMMARIZE = _env_bool("USE_BATCH_SUMMARIZE", True)
USE_SUMMARY_CACHE = _env_bool("USE_SUMMARY_CACHE", True)
USE_REGEX_FACTS = _env_bool("USE_REGEX_FACTS", True)

SUMMARY_CACHE_DIR = os.getenv("SUMMARY_CACHE_DIR", "./cache/summaries")
SUMMARY_TOP_N = _env_int("SUMMARY_TOP_N", 10)
RERANK_CANDIDATES_K = _env_int("RERANK_CANDIDATES_K", TOP_K * 2)
CONTEXT_TOP_N = _env_int("CONTEXT_TOP_N", 0)
LLM_ANSWER_TEMPERATURE = _env_float("LLM_ANSWER_TEMPERATURE", 0.1)


class SummaryCache:
    def __init__(self, cache_dir: str):
        self.cache_dir = cache_dir or "./cache/summaries"
        try:
            os.makedirs(self.cache_dir, exist_ok=True)
        except Exception:
            self.cache_dir = None

    def _make_key(self, filename: str, page: int, content: str) -> str:
        base = (filename or "").split("/")[-1]
        content_hash = hashlib.md5((content or "").encode("utf-8")).hexdigest()
        return f"{base}__p{int(page or 0)}__{content_hash[:12]}.json"

    def get(self, filename: str, page: int, content: str):
        if not self.cache_dir:
            return None
        path = os.path.join(self.cache_dir, self._make_key(filename, page, content))
        if not os.path.exists(path):
            return None
        try:
            with open(path, "r", encoding="utf-8") as f:
                return json.load(f)
        except Exception:
            return None

    def set(self, filename: str, page: int, content: str, payload: dict) -> None:
        if not self.cache_dir:
            return
        path = os.path.join(self.cache_dir, self._make_key(filename, page, content))
        try:
            data = dict(payload or {})
            data["_ts"] = time.time()
            with open(path, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False)
        except Exception:
            pass


def _search_with_retry(
    client,
    index: str,
    body: dict,
    max_retries: int = 3,
    backoff_base: float = 0.3,
    qid: str = "",
    tag: str = "",
):
    last_exception = None

    if isinstance(body, dict):
        if not tag:
            tag = str(body.get("_log_tag", "") or "")
        if not qid:
            qid = str(body.get("_log_qid", "") or "")

    try:
        size = int((body or {}).get("size", 0) or 0)
    except Exception:
        size = 0
    knn = (body or {}).get("knn") or {}
    try:
        knn_k = int(knn.get("k", 0) or 0)
    except Exception:
        knn_k = 0
    has_query = "query" in (body or {})
    has_knn = bool(knn)

    for attempt in range(1, max_retries + 1):
        attempt_start = time.time()
        if attempt == 1:
            logger.info(
                "[qid=%s] OceanBase HybridSearch.search start tag=%s index=%s size=%d has_query=%s has_knn=%s knn_k=%d",
                qid,
                tag,
                index,
                size,
                has_query,
                has_knn,
                knn_k,
            )

        try:
            res = client.search(index=index, body=body)
            elapsed = time.time() - attempt_start
            n = len(res) if isinstance(res, list) else -1
            logger.info(
                "[qid=%s] OceanBase HybridSearch.search ok tag=%s index=%s results=%d elapsed=%.3fs",
                qid,
                tag,
                index,
                n,
                elapsed,
            )
            if isinstance(res, list) and res:
                try:
                    r0 = res[0] if isinstance(res[0], dict) else {}
                    fn0 = os.path.basename(str(r0.get("filename", "") or ""))
                    pg0 = r0.get("page", 0)
                    sc0 = float(r0.get("_score", 0.0) or 0.0)
                    logger.info(
                        "[qid=%s] OceanBase HybridSearch.search top1 tag=%s file=%s page=%s score=%.4f",
                        qid,
                        tag,
                        fn0,
                        pg0,
                        sc0,
                    )
                except Exception:
                    pass
            return res
        except Exception as e:
            elapsed = time.time() - attempt_start
            last_exception = e
            if attempt >= max_retries:
                logger.error(
                    "[qid=%s] OceanBase HybridSearch.search failed tag=%s index=%s attempt=%d/%d elapsed=%.3fs err=%s",
                    qid,
                    tag,
                    index,
                    attempt,
                    max_retries,
                    elapsed,
                    e,
                    exc_info=True,
                )
                raise

            sleep_time = backoff_base * (2 ** (attempt - 1))
            logger.warning(
                "[qid=%s] OceanBase HybridSearch.search error tag=%s index=%s attempt=%d/%d elapsed=%.3fs err=%s, retrying in %.1fs",
                qid,
                tag,
                index,
                attempt,
                max_retries,
                elapsed,
                e,
                sleep_time,
            )
            time.sleep(sleep_time)

    if last_exception is not None:
        raise last_exception


def apply_rrf_reranking(search_results, k=60, score_weight=0.5):
    """
    Apply RRF-style reranking to search results.
    
    RRF (Reciprocal Rank Fusion) combines rank-based and score-based signals:
    - Rank signal: 1/(k + rank) - rewards top-ranked results
    - Score signal: normalized original score
    
    Args:
        search_results: List of search results with '_score' field
        k: RRF constant (typically 60), controls rank importance
        score_weight: Weight for original score (0-1), rank gets (1-score_weight)
    
    Returns:
        Reranked search results
    """
    if not search_results:
        return search_results
    
    # Normalize original scores to 0-1 range
    scores = [r.get('_score', 0.0) for r in search_results]
    max_score = max(scores) if scores else 1.0
    min_score = min(scores) if scores else 0.0
    score_range = max_score - min_score if max_score > min_score else 1.0
    
    # Calculate RRF scores
    rrf_results = []
    for rank, result in enumerate(search_results, 1):
        original_score = result.get('_score', 0.0)
        
        # Normalize score to 0-1
        normalized_score = (original_score - min_score) / score_range if score_range > 0 else 0.0
        
        # RRF rank component: 1/(k + rank)
        rank_score = 1.0 / (k + rank)
        
        # Combine rank and score signals
        rrf_score = score_weight * normalized_score + (1 - score_weight) * rank_score * 100
        
        # Create new result with RRF score
        new_result = result.copy()
        new_result['_original_score'] = original_score
        new_result['_score'] = rrf_score
        new_result['_rank'] = rank
        
        rrf_results.append(new_result)
    
    # Sort by new RRF score
    rrf_results.sort(key=lambda x: x['_score'], reverse=True)
    
    return rrf_results


def _extract_key_facts(text: str) -> str:
    if not text:
        return ""

    facts = []
    years = sorted(set(re.findall(r"(19\d{2}|20\d{2})", text)))
    if years:
        facts.append(f"年份: {', '.join(years[:10])}")

    percentages = list(dict.fromkeys(re.findall(r"\d+(?:\.\d+)?%", text)))
    if percentages:
        facts.append(f"百分比: {', '.join(percentages[:10])}")

    amounts = list(
        dict.fromkeys(
            re.findall(r"\d+(?:\.\d+)?(?:亿元|万元|万|亿|元)", text)
        )
    )
    if amounts:
        facts.append(f"金额: {', '.join(amounts[:10])}")

    numbers = sorted(set(re.findall(r"\d{3,}(?:\.\d+)?", text)))
    if numbers and len(numbers) <= 10:
        facts.append(f"关键数字: {', '.join(numbers[:10])}")

    return " | ".join(facts)


def llm_rerank_select(question: str, retrieval_results: list, top_k: int = 50, qid: str = "") -> tuple:
    """
    Use LLM to rerank top results and select the most relevant file and page.
    
    Args:
        question: User question
        retrieval_results: List of retrieval results with filename, page, score, content_preview
        top_k: Number of top results to send to LLM
    
    Returns:
        (filename, page): Most relevant file and page selected by LLM
    """
    if not retrieval_results:
        return "", 0

    candidates = [
        r
        for r in retrieval_results
        if r.get("used_for_context")
        and str(r.get("content_preview", "") or "").strip()
        and r.get("filename")
    ]
    if not candidates:
        candidates = retrieval_results

    n = min(top_k, len(candidates))
    hypo = [r for r in candidates if str(r.get("source") or "") == "hypothetical"]
    ques = [r for r in candidates if str(r.get("source") or "") == "question"]

    def _safe_rank(x: dict) -> int:
        try:
            return int(x.get("rank", 10**9) or 10**9)
        except Exception:
            return 10**9

    hypo.sort(key=_safe_rank)
    ques.sort(key=_safe_rank)

    take_h = min(len(hypo), n // 2)
    take_q = min(len(ques), n - take_h)
    if take_h + take_q < n:
        extra = n - (take_h + take_q)
        add_h = min(extra, len(hypo) - take_h)
        take_h += add_h
        extra -= add_h
        take_q += min(extra, len(ques) - take_q)

    raw_top_results = hypo[:take_h] + ques[:take_q]
    if len(raw_top_results) < n:
        rest = [r for r in candidates if r not in raw_top_results]
        rest.sort(key=_safe_rank)
        raw_top_results.extend(rest[: max(0, n - len(raw_top_results))])

    page_buckets: dict[tuple[str, int], dict] = {}
    for r in raw_top_results:
        fn_full = str(r.get("filename", "") or "")
        if not fn_full:
            continue
        try:
            pg = int(r.get("page", 0) or 0)
        except Exception:
            pg = 0
        try:
            sc = float(r.get("score", 0.0) or 0.0)
        except Exception:
            sc = 0.0
        text = str(r.get("content_preview", "") or "")
        try:
            rk = int(r.get("rank", 0) or 0)
        except Exception:
            rk = 0

        key = (fn_full, pg)
        bucket = page_buckets.get(key)
        if bucket is None:
            bucket = {
                "filename": fn_full,
                "page": pg,
                "score": sc,
                "best_rank": rk,
                "chunks": [],
            }
            page_buckets[key] = bucket
        else:
            bucket["score"] = max(float(bucket.get("score", 0.0) or 0.0), sc)
            bucket["best_rank"] = min(int(bucket.get("best_rank", rk) or rk), rk)

        bucket["chunks"].append({"score": sc, "rank": rk, "text": text})

    top_results: list[dict] = []
    for bucket in page_buckets.values():
        chunks = bucket.get("chunks") or []
        chunks = [c for c in chunks if (c.get("text") or "").strip()]
        chunks.sort(key=lambda c: (float(c.get("score", 0.0) or 0.0), -int(c.get("rank", 0) or 0)), reverse=True)
        top_texts = [str(c.get("text") or "") for c in chunks[:3] if str(c.get("text") or "").strip()]
        combined = "\n".join(top_texts).strip()
        bucket["content_preview"] = combined
        bucket["chunk_count"] = len(bucket.get("chunks") or [])
        top_results.append(bucket)

    if not top_results:
        return "", 0

    q_text = question or ""
    q_years = list(dict.fromkeys(re.findall(r"(19\d{2}|20\d{2})", q_text)))
    q_pcts = list(dict.fromkeys(re.findall(r"\d+(?:\.\d+)?%", q_text)))
    q_amounts = list(
        dict.fromkeys(re.findall(r"\d+(?:\.\d+)?(?:亿元|万元|万|亿|元)", q_text))
    )
    q_has_numbers = bool(q_years or q_pcts or q_amounts)
    q_is_causal = bool(re.search(r"(原因|为何|为什么|驱动|因素|关键|带来|促使|推动)", q_text))

    def _question_match_score(r: dict) -> int:
        text = str(r.get("content_preview", "") or "")
        s = 0
        for y in q_years:
            if y and y in text:
                s += 3
        for p in q_pcts:
            if p and p in text:
                s += 5
        for a in q_amounts:
            if a and a in text:
                s += 6
        return s

    def _causal_phrase_score(r: dict) -> int:
        text = str(r.get("content_preview", "") or "")
        if not text:
            return 0
        s = 0
        if "核心因素" in text:
            s += 8
        if "关键因素" in text:
            s += 8
        if "驱动因素" in text:
            s += 6
        if "主要原因" in text:
            s += 6
        if "原因在于" in text:
            s += 6
        if "关键在于" in text:
            s += 5
        if "原因" in text:
            s += 2
        return s

    def _sort_key(r: dict) -> tuple:
        base = (
            float(r.get("score", 0.0) or 0.0),
            -int(r.get("best_rank", 10**9) or 10**9),
            int(r.get("chunk_count", 0) or 0),
        )
        if q_has_numbers:
            return (_question_match_score(r),) + base
        if q_is_causal:
            return (_causal_phrase_score(r),) + base
        return (0,) + base

    top_results = sorted(top_results, key=_sort_key, reverse=True)
#    相似度分数: {score:.3f} 
    summaries: dict[int, str] = {}
    if USE_BATCH_SUMMARIZE and SUMMARY_TOP_N > 0:
        cache = SummaryCache(SUMMARY_CACHE_DIR) if USE_SUMMARY_CACHE else None
        to_summarize: list[tuple[int, str, int, str]] = []

        max_n = min(SUMMARY_TOP_N, len(top_results))
        for idx0, r in enumerate(top_results[:max_n], 1):
            content0 = str(r.get("content_preview", "") or "")
            if not content0:
                continue
            fn0 = str(r.get("filename", "") or "")
            try:
                pg0 = int(r.get("page", 0) or 0)
            except Exception:
                pg0 = 0

            cached = cache.get(fn0, pg0, content0) if cache else None
            if isinstance(cached, dict) and cached.get("summary"):
                summaries[idx0] = str(cached.get("summary") or "").strip()
            else:
                to_summarize.append((idx0, fn0, pg0, content0))

        if to_summarize:
            parts = []
            for idx0, _, _, content0 in to_summarize:
                excerpt = content0.replace("\n", " ").strip()[:300]
                parts.append(f"{idx0}) {excerpt}")

            batch_prompt = (
                f"请为下面按序号列出的 {len(to_summarize)} 个文档片段生成每个片段的一句摘要。"
                "输出必须为 JSON 数组，数组长度与输入片段数相同，每个项格式为："
                "{\"index\": <序号>, \"summary\": \"一句话摘要（提到关键数字/年份/金额，如有）\", \"key_numbers\": [..], \"confidence\": \"low|medium|high\"}。\n\n"
                "输入片段（请保持顺序）：\n"
                + "\n".join(parts)
                + "\n\n要求：不要添加额外说明，严格输出 JSON 数组。"
            )

            try:
                summary_resp = generate_llm_response(
                    [{"role": "user", "content": batch_prompt}],
                    model="qwen-max",
                    temperature=0.0,
                ).strip()

                arr_match = re.search(r"\[[\s\S]*\]", summary_resp)
                arr_text = arr_match.group(0) if arr_match else ""
                items = json.loads(arr_text) if arr_text else []

                if isinstance(items, list):
                    for it in items:
                        if not isinstance(it, dict):
                            continue
                        try:
                            i = int(it.get("index", 0) or 0)
                        except Exception:
                            i = 0
                        s = str(it.get("summary", "") or "").strip()
                        if i > 0 and s:
                            summaries[i] = s

                if cache:
                    for idx0, fn0, pg0, content0 in to_summarize:
                        s = summaries.get(idx0, "")
                        if s:
                            cache.set(fn0, pg0, content0, {"summary": s})
            except Exception:
                pass

    candidates = []
    for idx, r in enumerate(top_results, 1):
        filename = r.get("filename", "").split("/")[-1]
        page = r.get("page", 0)
        best_rank = int(r.get("best_rank", 0) or 0)
        score = float(r.get("score", 0.0) or 0.0)
        content = r.get("content_preview", "")
        key_facts = _extract_key_facts(content) if (USE_REGEX_FACTS and q_has_numbers) else ""
        summary = summaries.get(idx, "")
        snippet = (content or "").replace("\n", " ").strip()[:200]
        chunk_cnt = int(r.get("chunk_count", 0) or 0)

        candidates.append(
            f"{idx}) filename: {filename}, page: {page}, best_rank: {best_rank}, score: {score:.4f}, chunks: {chunk_cnt}, key_facts: {key_facts}, summary: {summary or snippet}"
        )
    
    candidates_text = "\n".join(candidates)
    
    if q_has_numbers:
        principles = (
            "1) 片段中包含与问题完全匹配的关键数字/年份/金额/百分比；\n"
            "2) 片段能直接回答问题；\n"
            "3) 如果多个片段都满足条件，选择信息最完整且更靠前的候选；"
        )
    else:
        principles = (
            "1) 片段能直接回答问题（优先选择出现‘核心因素/关键因素/驱动因素/主要原因/原因在于’等明确结论句的候选）；\n"
            "2) 如果多个候选都相关，优先选择与问题主体/范围最一致且结论最明确的候选；\n"
            "3) 如果问题未要求数字/年份/比例/金额，不要因为候选包含大量数字而优先选择不够直接的页面；"
        )

    # Build LLM prompt
    prompt = f"""你是一个精确的文档选择专家。下面给出 {len(top_results)} 个候选页面（按 filename+page 聚合；每个候选包含该页的若干片段信息），请严格从中选择一个最能回答问题的候选。

选择原则（优先级）：
{principles}

返回要求（严格）：
- 只返回一个 JSON 对象，不要输出任何其他文字：
{{"index": <候选序号>, "filename": "basename.pdf", "page": <页码>, "reason": "一句话说明依据（需引用候选中出现的短语或数字）"}}

候选列表：
{candidates_text}

问题：{question}
"""

    try:
        messages = [{"role": "user", "content": prompt}]
        result_text = generate_llm_response(messages, model="qwen-max", temperature=0.0).strip()  # Use qwen-max for better reranking
        
        # Extract JSON
        json_objs = re.findall(r"\{[\s\S]*?\}", result_text)
        parsed = None
        for obj in reversed(json_objs):
            try:
                cand = json.loads(obj)
                if isinstance(cand, dict) and any(k in cand for k in ["index", "filename", "page"]):
                    parsed = cand
                    break
            except Exception:
                continue

        if not parsed:
            logger.warning(
                "[qid=%s] LLM Rerank: invalid response format, falling back to hybrid strategy",
                qid,
            )
            return "", 0

        idx = 0
        try:
            idx = int(parsed.get("index", 0) or 0)
        except Exception:
            idx = 0

        filename = str(parsed.get("filename", "") or "").strip()
        try:
            page = int(parsed.get("page", 0) or 0)
        except Exception:
            page = 0

        def _norm_filename(s: str) -> str:
            return re.sub(r"\s+", "", str(s or ""))

        filename_norm = _norm_filename(filename)

        fp_choice = None
        fp_pos = None
        if filename_norm:
            for pos, r in enumerate(top_results, 1):
                cand_full = str(r.get("filename", "") or "")
                cand_base = cand_full.split("/")[-1] if cand_full else ""
                cand_page = int(r.get("page", 0) or 0)
                if _norm_filename(cand_base) != filename_norm:
                    continue
                if page and cand_page != page:
                    continue
                fp_choice = r
                fp_pos = pos
                break

            if fp_choice is None:
                for pos, r in enumerate(top_results, 1):
                    cand_full = str(r.get("filename", "") or "")
                    cand_base = cand_full.split("/")[-1] if cand_full else ""
                    if _norm_filename(cand_base) != filename_norm:
                        continue
                    fp_choice = r
                    fp_pos = pos
                    break

        idx_choice = None
        idx_pos = None
        if 1 <= idx <= len(top_results):
            idx_choice = top_results[idx - 1]
            idx_pos = idx

        chosen = None
        chosen_pos = None
        chosen_label = ""

        if fp_choice is not None and idx_choice is not None:
            if fp_choice is idx_choice:
                chosen = fp_choice
                chosen_pos = fp_pos
                chosen_label = "filename/page+index"
            else:
                if int(fp_pos or 10**9) <= int(idx_pos or 10**9):
                    chosen = fp_choice
                    chosen_pos = fp_pos
                    chosen_label = "filename/page"
                else:
                    chosen = idx_choice
                    chosen_pos = idx_pos
                    chosen_label = "index"
                logger.warning(
                    "[qid=%s] LLM Rerank: conflict between filename/page and index, resolved by %s",
                    qid,
                    chosen_label,
                )
        elif fp_choice is not None:
            chosen = fp_choice
            chosen_pos = fp_pos
            chosen_label = "filename/page"
        elif idx_choice is not None:
            chosen = idx_choice
            chosen_pos = idx_pos
            chosen_label = "index"

        if chosen is not None:
            try:
                best = top_results[0]
                best_score = float(best.get("score", 0.0) or 0.0)
                best_rank = int(best.get("best_rank", 10**9) or 10**9)
            except Exception:
                best = None
                best_score = 0.0
                best_rank = 10**9

            try:
                chosen_score = float(chosen.get("score", 0.0) or 0.0)
                chosen_rank = int(chosen.get("best_rank", 10**9) or 10**9)
            except Exception:
                chosen_score = 0.0
                chosen_rank = 10**9

            if best is not None and chosen_pos and chosen_pos > 10 and best_score > 0.0:
                ratio = chosen_score / best_score
                if ratio < 0.7 or chosen_rank > best_rank + 30:
                    logger.warning(
                        "[qid=%s] LLM Rerank: selected candidate seems weak (pos=%s score=%.4f rank=%s) vs best (score=%.4f rank=%s), overriding to best",
                        qid,
                        chosen_pos,
                        chosen_score,
                        chosen_rank,
                        best_score,
                        best_rank,
                    )
                    chosen = best
                    chosen_pos = 1
                    chosen_label = "best_override"

            chosen_fn = str(chosen.get("filename", "") or "")
            chosen_pg = int(chosen.get("page", 0) or 0)
            logger.info(
                "[qid=%s] LLM Rerank selected (%s): %s page %d",
                qid,
                chosen_label,
                os.path.basename(chosen_fn),
                chosen_pg,
            )
            return chosen_fn, chosen_pg

        logger.warning(
            "[qid=%s] LLM Rerank: could not match parsed selection, falling back",
            qid,
        )
        return "", 0
            
    except Exception as e:
        logger.error(
            "[qid=%s] LLM Rerank error: %s, falling back to hybrid strategy",
            qid,
            e,
            exc_info=True,
        )
        return "", 0


def search(question: str) -> Answer:
    """
    Query the RAG system with a question and return an answer.

    Args:
        question: The question to answer

    Returns:
        Answer object containing the question, answer, filename, and page
    """
    answer = Answer(
        question=question,
    )

    qid = hashlib.md5((question or "").encode("utf-8")).hexdigest()[:8]
    logger.info(
        "[qid=%s] Search start: %s",
        qid,
        (question or "").replace("\n", " ")[:120],
    )

    logger.debug(f"Processing query: '{question[:100]}...'")

    try:
        # Step 1: Generate hypothetical answer first (HyDE approach)
        logger.debug("Generating hypothetical answer for better retrieval...")
        hypothetical_prompt = (
            "你是一个针对公司财务/业务文档的检索辅助生成器。"
            "请基于任务对下面的问题生成一个假设性回答（可能的答案短文本），并抽取最多 8 个与检索最相关的实体或关键词。\n\n"
            "要求：\n"
            "1) 只能以严格 JSON 格式返回，且不要输出除 JSON 以外的任何文字。\n"
            "2) JSON 模板如下：\n"
            "{\n"
            "  \"answer\": \"<简短的假设性回答，少于300字；若无相关信息可写空字符串>\",\n"
            "  \"entities\": [\"实体1\", \"实体2\"],\n"
            "  \"keywords\": [\"关键词1\", \"关键词2\"]\n"
            "}\n\n"
            "说明：不要凭空编造具体数字或年份。如果推测，请将推测放在 answer 中并在句首标注[推测]。\n\n"
            f"问题：{question}\n"
        )

        hypothetical_messages = [{"role": "user", "content": hypothetical_prompt}]
        hypothetical_raw = generate_llm_response(hypothetical_messages, temperature=0).strip()
        logger.info(f"Hypothetical raw response: {hypothetical_raw[:1000]}...")

        # Default fallback: use raw text for both embedding and FTS if parsing fails
        hypothetical_answer = hypothetical_raw
        hypothetical_terms = hypothetical_raw

        try:
            # Try to extract JSON object from the response
            json_match = re.search(r"\{.*\}", hypothetical_raw, re.S)
            json_text = json_match.group(0) if json_match else hypothetical_raw
            data = json.loads(json_text)

            # Extract answer text
            answer_text = str(data.get("answer", "")).strip()
            if answer_text:
                hypothetical_answer = answer_text

            # Extract entities and keywords as flat list of strings
            entities = data.get("entities") or []
            keywords = data.get("keywords") or []

            if not isinstance(entities, list):
                entities = [entities]
            if not isinstance(keywords, list):
                keywords = [keywords]

            term_list = [str(t).strip() for t in entities + keywords if str(t).strip()]
            if term_list:
                hypothetical_terms = " ".join(term_list)

            logger.info(
                "Parsed HyDE JSON successfully: answer_len=%d, terms=%s",
                len(hypothetical_answer),
                ", ".join(term_list[:10]),
            )
        except Exception as parse_err:
            logger.warning(
                "Failed to parse HyDE JSON, falling back to raw text. Error: %s",
                parse_err,
            )

        # Save hypothetical answer to Answer object for later inspection
        answer.hypothetical_answer = hypothetical_answer

        # Initialize OceanBase client
        client = get_or_create_client()

        # Step 2: Dual-path retrieval - retrieve separately with hypothetical answer and original question
        # Then merge results to leverage both semantic expansion and keyword precision
        logger.info("Starting dual-path retrieval...")

        # Path 1: Retrieve with hypothetical answer (semantic-focused)
        logger.debug("Path 1: Retrieving with hypothetical answer...")
        hypo_embedding = generate_embedding(hypothetical_answer, dimensions=2048)

        hypo_fts_query = {
            "bool": {
                "should": [
                    {
                        "query_string": {
                            "fields": ["content"],
                            "type": "best_fields",
                            "query": hypothetical_terms,
                        }
                    }
                ],
            }
        }
        
        hypo_search_request = {
            "query": hypo_fts_query,
            "knn": {
                "field": "vector",
                "k": TOP_K,
                "query_vector": hypo_embedding,
            },
            "from": 0,
            "size": TOP_K,
        }
        
        hypo_results = _search_with_retry(
            client,
            index=TABLE_NAME,
            body=hypo_search_request,
            qid=qid,
            tag="hypothetical",
        )
        logger.info(
            "[qid=%s] Hypothetical answer retrieval: found %d results",
            qid,
            len(hypo_results) if isinstance(hypo_results, list) else -1,
        )
        
        # Path 2: Retrieve with original question (keyword-focused)
        logger.debug("Path 2: Retrieving with original question...")
        question_embedding = generate_embedding(question, dimensions=2048)
        
        question_fts_query = {
            "bool": {
                "should": [
                    {
                        "query_string": {
                            "fields": ["content"],
                            "type": "best_fields",
                            "query": question,
                        }
                    }
                ],
            }
        }
        
        question_search_request = {
            "query": question_fts_query,
            "knn": {
                "field": "vector",
                "k": TOP_K,
                "query_vector": question_embedding,
            },
            "from": 0,
            "size": TOP_K,
        }
        
        question_results = _search_with_retry(
            client,
            index=TABLE_NAME,
            body=question_search_request,
            qid=qid,
            tag="question",
        )
        logger.info(
            "[qid=%s] Original question retrieval: found %d results",
            qid,
            len(question_results) if isinstance(question_results, list) else -1,
        )
        
        # Step 3: Tag each result with its source path
        for result in hypo_results:
            result['_source'] = 'hypothetical'
        for result in question_results:
            result['_source'] = 'question'

        # For logging and sanity checks, still build merged list
        search_results = hypo_results + question_results
        logger.info(
            "[qid=%s] Merged retrieval: %d total results (hypo: %d, question: %d)",
            qid,
            len(search_results),
            len(hypo_results),
            len(question_results),
        )

        if not search_results or len(search_results) == 0:
            logger.error(
                f"No search results found for question: '{question[:50]}...'"
            )
            raise RuntimeError("No search results found for the given question")

        logger.debug(f"Final merged results: {len(search_results)} documents")
        
        # RRF reranking disabled - using hybrid strategy instead
        # search_results = apply_rrf_reranking(search_results, k=60)

        # Extract relevant context from search results
        contexts = []
        retrieval_results = []  # Store detailed retrieval info

        # Use TOP_K from each path for context extraction (HyDE TOP_K + question TOP_K)
        rank_counter = 1

        # Path 1 contexts: hypothetical results
        for result in hypo_results[:TOP_K]:
            content = result.get("content", "")
            filename = result.get("filename", "")
            page = result.get("page", 0)
            score = result.get("_score", 0.0)
            source = result.get("_source", "hypothetical")

            if content:
                context_index = len(contexts)
                contexts.append(content)
                retrieval_results.append({
                    "rank": rank_counter,
                    "filename": filename,
                    "page": page,
                    "score": score,
                    "source": source,
                    "content_preview": content[:300] + "..." if len(content) > 300 else content,
                    "content_length": len(content),
                    "context_index": context_index,
                    "used_for_context": True,
                })
                rank_counter += 1

        # Path 2 contexts: original question results
        for result in question_results[:TOP_K]:
            content = result.get("content", "")
            filename = result.get("filename", "")
            page = result.get("page", 0)
            score = result.get("_score", 0.0)
            source = result.get("_source", "question")

            if content:
                context_index = len(contexts)
                contexts.append(content)
                retrieval_results.append({
                    "rank": rank_counter,
                    "filename": filename,
                    "page": page,
                    "score": score,
                    "source": source,
                    "content_preview": content[:300] + "..." if len(content) > 300 else content,
                    "content_length": len(content),
                    "context_index": context_index,
                    "used_for_context": True,
                })
                rank_counter += 1

        # Also save the remaining results (beyond TOP_K of each path) for analysis purposes
        for result in hypo_results[TOP_K:]:
            filename = result.get("filename", "")
            page = result.get("page", 0)
            score = result.get("_score", 0.0)
            source = result.get("_source", "hypothetical")

            retrieval_results.append({
                "rank": rank_counter,
                "filename": filename,
                "page": page,
                "score": score,
                "source": source,
                "content_preview": "",
                "content_length": 0,
                "used_for_context": False,
            })
            rank_counter += 1

        for result in question_results[TOP_K:]:
            filename = result.get("filename", "")
            page = result.get("page", 0)
            score = result.get("_score", 0.0)
            source = result.get("_source", "question")

            retrieval_results.append({
                "rank": rank_counter,
                "filename": filename,
                "page": page,
                "score": score,
                "source": source,
                "content_preview": "",
                "content_length": 0,
                "used_for_context": False,
            })
            rank_counter += 1

        if not contexts:
            logger.error(
                f"No valid contexts extracted from search results for question: '{question[:50]}...'"
            )
            raise RuntimeError("No valid contexts extracted from search results")

        logger.debug(
            f"Extracted {len(contexts)} valid contexts, total length: {sum(len(c) for c in contexts)} chars"
        )

        # Combine contexts
        context_text = "\n\n".join(contexts)

        # File and page selection strategy
        from collections import defaultdict, Counter
        
        if retrieval_results:
            use_hybrid_strategy = True  # Default to hybrid strategy
            
            if USE_LLM_RERANK:
                # Strategy: LLM Rerank - Let LLM select the most relevant file and page
                logger.info("[qid=%s] Using LLM Rerank to select file and page", qid)
                llm_k = min(TOP_K * 2, RERANK_CANDIDATES_K) if RERANK_CANDIDATES_K > 0 else TOP_K * 2
                llm_filename, llm_page = llm_rerank_select(question, retrieval_results, top_k=llm_k, qid=qid)
                
                if llm_filename:
                    answer.filename = llm_filename
                    answer.page = llm_page
                    use_hybrid_strategy = False  # LLM succeeded, skip hybrid
                    logger.info(
                        "[qid=%s] LLM Rerank succeeded: %s page %d",
                        qid,
                        os.path.basename(str(llm_filename or "")),
                        int(llm_page or 0),
                    )
                else:
                    # Fall back to hybrid strategy if LLM fails
                    logger.warning(
                        "[qid=%s] LLM Rerank failed, falling back to hybrid strategy",
                        qid,
                    )
            
            if use_hybrid_strategy:
                # Strategy: Hybrid (Average score filtering + Top3 voting)
                # Step 1: Use average score to select Top3 files
                # Step 2: In Top3 files' chunks, use majority voting to select final file and page
                
                # Step 1: Calculate average score for each filename
                file_stats = defaultdict(lambda: {'total_score': 0.0, 'count': 0})
                for r in retrieval_results:
                    if r.get('filename') and r.get('score'):
                        file_stats[r['filename']]['total_score'] += r['score']
                        file_stats[r['filename']]['count'] += 1
                
                # Get Top3 files by average score
                if file_stats:
                    file_avg_scores = {
                        fname: stats['total_score'] / stats['count'] 
                        for fname, stats in file_stats.items() if stats['count'] > 0
                    }
                    sorted_files = sorted(file_avg_scores.items(), key=lambda x: x[1], reverse=True)
                    top3_files = [f[0] for f in sorted_files[:3]]  # Get top 3 file names
                    try:
                        top3_bases = [os.path.basename(str(x or "")) for x in top3_files]
                        logger.debug("[qid=%s] Hybrid strategy top3_files=%s", qid, top3_bases)
                    except Exception:
                        pass
                    
                    # Step 2: In Top3 files, use majority voting (count chunks)
                    top3_chunks = [r for r in retrieval_results if r.get('filename') in top3_files]
                    
                    if top3_chunks:
                        # Vote for filename by chunk count
                        filename_counter = Counter(r.get('filename') for r in top3_chunks if r.get('filename'))
                        if filename_counter:
                            answer.filename = filename_counter.most_common(1)[0][0]
                            
                            # Vote for page within selected file
                            file_chunks = [r for r in top3_chunks if r.get('filename') == answer.filename]
                            if file_chunks:
                                page_counter = Counter(r.get('page') for r in file_chunks if r.get('page') is not None)
                                if page_counter:
                                    answer.page = page_counter.most_common(1)[0][0]

                    if answer.filename:
                        logger.debug(
                            "[qid=%s] Hybrid strategy selected: %s page %s",
                            qid,
                            os.path.basename(str(answer.filename or "")),
                            answer.page,
                        )

        # Save retrieval results to answer object
        answer.retrieval_results = retrieval_results

        if USE_LLM_RERANK and answer.filename and not use_hybrid_strategy:
            try:
                selected_indices = [
                    int(r.get("context_index"))
                    for r in retrieval_results
                    if r.get("used_for_context")
                    and r.get("filename") == answer.filename
                    and r.get("page") == answer.page
                    and r.get("context_index") is not None
                ]
                selected_set = set(selected_indices)

                if selected_indices:
                    contexts = [
                        contexts[i]
                        for i in selected_indices
                        if isinstance(i, int) and 0 <= i < len(contexts)
                    ] + [c for i, c in enumerate(contexts) if i not in selected_set]

                logger.info(
                    "Reordered contexts: selected_first=%d total_contexts=%d",
                    len(selected_indices),
                    len(contexts),
                )
            except Exception as e:
                logger.debug("Reordering contexts skipped: %s", e)

        if CONTEXT_TOP_N > 0 and len(contexts) > CONTEXT_TOP_N:
            contexts = contexts[:CONTEXT_TOP_N]
        
        # Combine contexts
        context_text = "\n\n".join(contexts)

        # Generate answer using LLM
        logger.debug(f"Generating answer using LLM model ...")
        prompt = QUERY_USER_PROMPT_TEMPLATE.format(
            context_text=context_text,
            question=question,
        )

        messages = [
            {
                "role": "system",
                "content": QUERY_SYSTEM_PROMPT,
            },
            {"role": "user", "content": prompt},
        ]
        # answer.answer = generate_llm_response(messages,model="qwen-max", temperature=0.3)
        answer.answer = generate_llm_response(messages, temperature=LLM_ANSWER_TEMPERATURE)  # Use 0.3 for natural answer generation
        logger.debug(
            f"Query completed: question='{question[:50]}...', filename='{answer.filename}', page={answer.page}"
        )

    except Exception as e:
        logger.error(f"Error in query: {e}", exc_info=True)
        raise

    return answer
