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
    dimensions: int = 2048,  # Default 2048 for highest precision
    wait_seconds: float = 2.0,
) -> List[float]:
    """
    Generate text embeddings, retry indefinitely on transient errors.
    
    Args:
        embedding_text: Text to embed
        model: Embedding model name
        dimensions: Vector dimensions (supports 512/768/1024/1536/2048)
        wait_seconds: Wait time between retries
    
    Returns:
        Embedding vector as list of floats
    """
    max_retries = 30
    
    for retry_count in range(1, max_retries + 1):
        try:
            resp = embedding_client.embeddings.create(
                model=model,
                input=embedding_text,
                dimensions=dimensions,
            )
            
            if not resp.data or len(resp.data) == 0:
                raise ValueError("Empty response from embedding API")
            
            vector = resp.data[0].embedding
            if not vector:
                raise ValueError("Empty embedding vector")
                
            return vector
            
        except (RateLimitError, APIConnectionError) as err:
            if retry_count >= max_retries:
                logger.error(f"[Embedding] Failed after {max_retries} retries: {err}")
                raise
            logger.warning(f"[Embedding retry {retry_count}/{max_retries}] {err.__class__.__name__}, retrying in {wait_seconds}s")
            time.sleep(wait_seconds)
            
        except APIError as err:
            code = getattr(err, "status_code", None)
            if code and code >= 500:
                if retry_count >= max_retries:
                    logger.error(f"[Embedding] Failed after {max_retries} retries: {err}")
                    raise
                logger.warning(f"[Embedding retry {retry_count}/{max_retries}] Server error {code}, retrying in {wait_seconds}s")
                time.sleep(wait_seconds)
            else:
                raise
                
        except Exception:
            raise


if __name__ == "__main__":
    result = generate_response("Hello, world!")
    print(f"Embedding dimension: {len(result)}")
    print(f"First 5 values: {result[:5]}")
