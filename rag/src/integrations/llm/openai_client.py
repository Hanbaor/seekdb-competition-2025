import os
import time
from typing import Dict, List

import dotenv
from openai import APIConnectionError, APIError, OpenAI, RateLimitError

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
    retry_interval: float = 2.0,  # 固定重试间隔
    timeout: float = 300.0,
    temperature: float = 0.0,
) -> str:
    """
    Generate a response from the LLM with error handling and infinite retry logic.

    Args:
        messages: List of message dictionaries with 'role' and 'content' keys
        model: Model name to use for generation
        retry_interval: Fixed interval between retries in seconds
        timeout: Request timeout in seconds
        temperature: Sampling temperature

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

        except (RateLimitError, APIConnectionError, TimeoutError) as e:
            print(f"LLM error (attempt {attempt}): {type(e).__name__}. Retrying in {retry_interval}s...")
            time.sleep(retry_interval)

        except APIError as e:
            if e.status_code and e.status_code >= 500:
                print(f"LLM server error {e.status_code} (attempt {attempt}). Retrying in {retry_interval}s...")
                time.sleep(retry_interval)
            else:
                raise e

        except Exception as e:
            raise e


if __name__ == "__main__":
    response = generate_response(
        messages=[
            {"role": "system", "content": "You are a helpful assistant."},
            {"role": "user", "content": "Who are you?"},
        ]
    )

    print(response)
