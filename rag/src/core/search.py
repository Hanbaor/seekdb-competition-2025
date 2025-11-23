import logging
import os
import time

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
TOP_K = 50  # Optimal value for LLM Rerank - 65.29 best score

# Enable LLM Rerank: Use LLM to rerank top results and select best file/page
USE_LLM_RERANK = True  # Set to True to enable LLM reranking


def _search_with_retry(client, index: str, body: dict, max_retries: int = 3, backoff_base: float = 0.5):
    """Execute client.search with simple retry and exponential backoff.

    This is mainly to handle transient errors from OceanBase / network.
    """
    last_exception = None
    for attempt in range(1, max_retries + 1):
        try:
            return client.search(index=index, body=body)
        except Exception as e:  # noqa: BLE001 - we want to log and retry all exceptions here
            last_exception = e
            if attempt >= max_retries:
                logger.error(
                    "Hybrid search failed after %d attempts: %s", attempt, e, exc_info=True
                )
                raise

            sleep_time = backoff_base * (2 ** (attempt - 1))
            logger.warning(
                "Hybrid search error on attempt %d/%d: %s, retrying in %.1fs",
                attempt,
                max_retries,
                e,
                sleep_time,
            )
            time.sleep(sleep_time)

    # Should not reach here, but keep for type checkers
    if last_exception is not None:
        raise last_exception
    return []


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


def llm_rerank_select(question: str, retrieval_results: list, top_k: int = 50) -> tuple:
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
    
    # Only use top K results
    top_results = retrieval_results[:min(top_k, len(retrieval_results))]
#    相似度分数: {score:.3f} 
    # Build candidate list
    candidates = []
    for idx, r in enumerate(top_results, 1):
        filename = r.get('filename', '').split('/')[-1]  # Only filename
        page = r.get('page', 0)
        content = r.get('content_preview', '')
        score = r.get('score', 0)
        
        candidates.append(f"""【候选{idx}】
文件: {filename}
页码: 第{page}页
内容摘要: {content}""")
    
    candidates_text = "\n\n".join(candidates)
    
    # Build LLM prompt
    prompt = f"""你是一个专业的金融文档检索评估专家。现在有一个关于千味央厨公司的问题和{len(top_results)}个通过混合检索筛选出的候选文档片段。

问题：{question}

候选文档片段（共{len(top_results)}个）：
{candidates_text}

请仔细分析每个候选片段的内容，判断哪一个最能准确、完整地回答这个问题。或者和问题最相关

要求：
1. 必须从上述候选中选择一个
2. 只返回一个JSON对象：{{"filename": "完整文件名.pdf", "page": 页码数字, "reason": "选择理由（一句话）"}}
3. filename必须与候选中的文件名完全一致，页码也一致并且是对应的
4. 不要有任何其他解释文字

返回："""

    try:
        messages = [{"role": "user", "content": prompt}]
        result_text = generate_llm_response(messages, temperature=0.0).strip()  # Use 0.0 for deterministic reranking
        
        # Extract JSON
        import re
        import json
        json_match = re.search(r'\{[^}]+\}', result_text)
        if json_match:
            result = json.loads(json_match.group())
            filename = result.get('filename', '')
            page = result.get('page', 0)
            
            # Find full file path
            for r in top_results:
                if filename in r.get('filename', ''):
                    logger.info(f"LLM Rerank selected: {filename} page {page}")
                    return r.get('filename', ''), page
            
            # If not found, fall back to hybrid strategy
            logger.warning(f"LLM Rerank: filename '{filename}' not found in results, triggering fallback")
            return "", 0
        else:
            logger.warning(f"LLM Rerank: invalid response format, falling back to hybrid strategy")
            return "", 0
            
    except Exception as e:
        logger.error(f"LLM Rerank error: {e}, falling back to hybrid strategy")
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

    logger.debug(f"Processing query: '{question[:100]}...'")

    try:
        # Step 1: Generate hypothetical answer first (HyDE approach)
        logger.debug("Generating hypothetical answer for better retrieval...")
        hypothetical_prompt = f"""请基于你的知识，对以下问题给出一个简短的假设性答案（1-2句话即可）。
不需要完全准确，只需要包含可能相关的关键词和概念。

问题：{question}

假设答案："""
        
        hypothetical_messages = [{"role": "user", "content": hypothetical_prompt}]
        hypothetical_answer = generate_llm_response(hypothetical_messages, temperature=0.3).strip()
        logger.info(f"Hypothetical answer: {hypothetical_answer[:100]}...")
        
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
                            "query": hypothetical_answer,
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

        hypo_results = _search_with_retry(client, index=TABLE_NAME, body=hypo_search_request)
        logger.info(f"Hypothetical answer retrieval: found {len(hypo_results)} results")
        
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
        
        question_results = _search_with_retry(client, index=TABLE_NAME, body=question_search_request)
        logger.info(f"Original question retrieval: found {len(question_results)} results")
        
        # Step 3: Tag each result with its source path before merging
        for result in hypo_results:
            result["_source"] = "hypothetical"
        for result in question_results:
            result["_source"] = "question"
        
        # Simply concatenate both result lists (no deduplication)
        # Let LLM Rerank handle the larger candidate pool
        search_results = hypo_results + question_results
        
        logger.info(
            f"Merged retrieval: {len(search_results)} total results (hypo: {len(hypo_results)}, question: {len(question_results)})"
        )

        if not search_results or len(search_results) == 0:
            logger.warning(
                f"No search results found for question: '{question[:50]}...'"
            )
            answer.answer = "抱歉，我没有找到相关的信息来回答这个问题。"
            return answer

        logger.debug(f"Final merged results: {len(search_results)} documents")
        
        # RRF reranking disabled - using hybrid strategy instead
        # search_results = apply_rrf_reranking(search_results, k=60)

        # Extract relevant context from search results
        contexts = []
        retrieval_results = []  # Store detailed retrieval info

        # Use TOP_K for context extraction but save all merged results for analysis
        for idx, result in enumerate(search_results[:TOP_K]):
            content = result.get("content", "")
            filename = result.get("filename", "")
            page = result.get("page", 0)
            score = result.get("_score", 0.0)  # Hybrid search score
            source = result.get("_source", "unknown")  # Track which retrieval path this came from

            if content:
                contexts.append(content)
                # Save detailed retrieval result
                retrieval_results.append({
                    "rank": idx + 1,
                    "filename": filename,
                    "page": page,
                    "score": score,
                    "source": source,  # Track retrieval path (hypothetical/question)
                    "content_preview": content[:200] + "..." if len(content) > 200 else content,
                    "content_length": len(content),
                    "used_for_context": True  # Mark as used in LLM context
                })
        
        # Also save the remaining results (beyond TOP_K) for analysis purposes
        for idx, result in enumerate(search_results[TOP_K:], start=TOP_K):
            filename = result.get("filename", "")
            page = result.get("page", 0)
            score = result.get("_score", 0.0)
            source = result.get("_source", "unknown")
            
            retrieval_results.append({
                "rank": idx + 1,
                "filename": filename,
                "page": page,
                "score": score,
                "source": source,
                "content_preview": "",  # Don't store content for analysis-only results
                "content_length": 0,
                "used_for_context": False  # Not used in LLM context
            })

        if not contexts:
            logger.warning(
                f"No valid contexts extracted from search results for question: '{question[:50]}...'"
            )
            answer.answer = "抱歉，我没有找到相关的信息来回答这个问题。"
            return answer

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
                logger.info("Using LLM Rerank to select file and page")
                llm_filename, llm_page = llm_rerank_select(question, retrieval_results, top_k=50)
                
                if llm_filename:
                    answer.filename = llm_filename
                    answer.page = llm_page
                    use_hybrid_strategy = False  # LLM succeeded, skip hybrid
                    logger.info(f"LLM Rerank succeeded: {llm_filename} page {llm_page}")
                else:
                    # Fall back to hybrid strategy if LLM fails
                    logger.warning("LLM Rerank failed, falling back to hybrid strategy")
            
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

        # Save retrieval results to answer object
        answer.retrieval_results = retrieval_results

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

        answer.answer = generate_llm_response(messages, temperature=0.3)  # Use 0.3 for natural answer generation
        logger.debug(
            f"Query completed: question='{question[:50]}...', filename='{answer.filename}', page={answer.page}"
        )

    except Exception as e:
        logger.error(f"Error in query: {e}", exc_info=True)
        answer.answer = f"查询过程中发生错误：{str(e)}"

    return answer
