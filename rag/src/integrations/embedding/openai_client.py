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
    retry_interval: float = 2.0,  # 固定重试间隔
) -> List[float]:
    """
    Generate embeddings from text using OpenAI-compatible API with infinite retry logic.

    Args:
        embedding_text: Text to generate embeddings for
        model: Model name to use for embedding generation
        dimensions: Output dimension size
        retry_interval: Fixed interval between retries in seconds

    Returns:
        The embedding vector as a list of floats
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

        except Exception as e:
            # Retry indefinitely on any error until successful
            logger.warning(f"Embedding error (attempt {attempt}): {type(e).__name__}: {str(e)[:100]}. Retrying in {retry_interval}s...")
            time.sleep(retry_interval)


if __name__ == "__main__":
    result = generate_response("Hello, world!")
    print(f"Embedding dimension: {len(result)}")
    print(f"First 5 values: {result[:5]}")
