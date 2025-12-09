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
    model: str = "qwen-plus",
    timeout: float = 300.0,
    temperature: float = 0.0,
    wait_seconds: float = 2.0,
) -> str:
    """
    Call LLM to generate response, retry indefinitely on transient errors.
    
    Args:
        messages: List of message dicts
        model: Model name
        timeout: Request timeout in seconds
        temperature: Sampling temperature
        wait_seconds: Wait time between retries
    
    Returns:
        Generated response text
    """
    retry_count = 0
    
    while True:
        retry_count += 1
        try:
            result = llm.chat.completions.create(
                model=model,
                messages=messages,
                temperature=temperature,
                timeout=timeout,
            )
            
            content = result.choices[0].message.content if result.choices else None
            if not content:
                raise ValueError("Empty response from API")
            
            return content
            
        except (RateLimitError, APIConnectionError, TimeoutError) as err:
            print(f"[Retry {retry_count}] {err.__class__.__name__}: retrying in {wait_seconds}s...")
            time.sleep(wait_seconds)
            
        except APIError as err:
            # Server errors are retryable, client errors are not
            if err.status_code and err.status_code >= 500:
                print(f"[Retry {retry_count}] Server error {err.status_code}: retrying in {wait_seconds}s...")
                time.sleep(wait_seconds)
            else:
                raise
                
        except Exception:
            raise


if __name__ == "__main__":
    response = generate_response(
        messages=[
            {"role": "system", "content": "You are a helpful assistant."},
            {"role": "user", "content": "Who are you?"},
        ]
    )

    print(response)
