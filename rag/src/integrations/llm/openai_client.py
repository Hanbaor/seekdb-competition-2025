import logging
import os
import time
from typing import Dict, List

import dotenv
from openai import APIConnectionError, APIError, OpenAI, RateLimitError

logger = logging.getLogger(__name__)

dotenv.load_dotenv()

OPENAI_BASE_URL = os.getenv(
    "OPENAI_BASE_URL",
    "https://dashscope-intl.aliyuncs.com/compatible-mode/v1",
)

OPENAI_API_KEY = os.getenv(
    "OPENAI_API_KEY",
    "",
)

llm = OpenAI(
    api_key=OPENAI_API_KEY,
    base_url=OPENAI_BASE_URL,
)


def generate_response(
    messages: List[Dict[str, str]],
    model="qwen-plus",
    retry_interval: float = 2.0,  # delay between retry attempts
    timeout: float = 300.0,
    temperature: float = 0.0,
) -> str:
    """
    Send chat messages to LLM and retrieve completion.
    Keeps retrying on failure until a valid response is obtained.

    Args:
       messages: Conversation history as list of role/content dicts
        model: LLM model identifier to invoke
        retry_interval: Wait duration before retrying (seconds)
        timeout: Max wait time for API response
        temperature: Controls randomness in output (0=deterministic)

    Returns:
        The generated response content as a string
    """
    attempt = 0
    while True:
        attempt += 1
        try:
            response = llm.chat.completions.create(
                model=model,
                messages=messages,
                temperature=temperature,
                timeout=timeout,
            )

            if not response.choices or not response.choices[0].message.content:
                raise ValueError("Empty response from API")

            return response.choices[0].message.content

        except (APIConnectionError, RateLimitError) as e:
            # 网络错误、限流：无限重试
            logger.warning(f"LLM error (attempt {attempt}): {type(e).__name__}: {str(e)[:100]}. Retrying in {retry_interval}s...")
            time.sleep(retry_interval)
        except APIError as e:
            # API 错误：5xx 服务端错误重试，4xx 客户端错误直接抛出
            if hasattr(e, 'status_code') and e.status_code >= 500:
                logger.warning(f"LLM server error (attempt {attempt}): {e}. Retrying in {retry_interval}s...")
                time.sleep(retry_interval)
            else:
                logger.error(f"LLM API error (non-retryable): {e}")
                raise
        except Exception as e:
            # 其他未知错误：直接抛出
            logger.error(f"LLM unexpected error: {type(e).__name__}: {e}")
            raise


if __name__ == "__main__":
    response = generate_response(
        messages=[
            {"role": "system", "content": "You are a helpful assistant."},
            {"role": "user", "content": "Who are you?"},
        ]
    )

    print(response)
