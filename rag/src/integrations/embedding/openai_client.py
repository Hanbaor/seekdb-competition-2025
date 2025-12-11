import logging
import os
import time
from typing import List

import dotenv
from openai import APIConnectionError, APIError, OpenAI, RateLimitError

dotenv.load_dotenv()

logger = logging.getLogger(__name__)

OPENAI_BASE_URL = os.getenv(
    "OPENAI_BASE_URL",
    "https://dashscope-intl.aliyuncs.com/compatible-mode/v1",
)

OPENAI_API_KEY = os.getenv(
    "OPENAI_API_KEY",
    "",
)

# Initialize OpenAI client
embedding_client = OpenAI(
    api_key=OPENAI_API_KEY,
    base_url=OPENAI_BASE_URL,
)


def generate_response(
    embedding_text: str,
    model: str = "text-embedding-v4",
    dimensions: int = 2048,  # 2048 dims for best accuracy
    retry_interval: float = 2.0,  # wait time between retries
) -> List[float]:
    """
    Convert input text into vector representation via embedding API.
    Automatically retries on failure until success.

    Args:
        embedding_text: Source text to be vectorized
        model: Embedding model identifier
        dimensions: Vector output length
        retry_interval: Seconds to wait before next attempt

    Returns:
        List of floats representing the text embedding
    """
    attempt = 0
    while True:
        attempt += 1
        try:
            response = embedding_client.embeddings.create(
                model=model,
                input=embedding_text,
                dimensions=dimensions,
            )

            if not response or not response.data or len(response.data) == 0:
                raise ValueError("Empty or invalid response from OpenAI API")

            embedding_vector = response.data[0].embedding
            if not embedding_vector or len(embedding_vector) == 0:
                raise ValueError("Empty embedding vector from API")

            return embedding_vector

        except (APIConnectionError, RateLimitError) as e:
            # 网络错误、限流：无限重试
            logger.warning(f"Embedding error (attempt {attempt}): {type(e).__name__}: {str(e)[:100]}. Retrying in {retry_interval}s...")
            time.sleep(retry_interval)
        except APIError as e:
            # API 错误：5xx 服务端错误重试，4xx 客户端错误直接抛出
            if hasattr(e, 'status_code') and e.status_code >= 500:
                logger.warning(f"Embedding server error (attempt {attempt}): {e}. Retrying in {retry_interval}s...")
                time.sleep(retry_interval)
            else:
                logger.error(f"Embedding API error (non-retryable): {e}")
                raise
        except Exception as e:
            # 其他未知错误：直接抛出
            logger.error(f"Embedding unexpected error: {type(e).__name__}: {e}")
            raise


if __name__ == "__main__":
    result = generate_response("Hello, world!")
    print(f"Embedding dimension: {len(result)}")
    print(f"First 5 values: {result[:5]}")
