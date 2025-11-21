import os
import asyncio
from openai import AsyncOpenAI
import logging
from tenacity import (
    retry,
    stop_after_attempt,
    wait_exponential,
    retry_if_exception_type,
    before_sleep_log
)
from openai import APIError, RateLimitError, APIConnectionError

logger = logging.getLogger(__name__)

class APIClient:
    def __init__(self, config):
        self.api_key = os.getenv("OPENAI_API_KEY")
        self.base_url = os.getenv("OPENAI_BASE_URL")
        self.model = config.get("vlm", {}).get("model_name", "qwen3-vl-flash")
        
        if not self.api_key:
            logger.warning("OPENAI_API_KEY not set")
            
        self.client = AsyncOpenAI(
            api_key=self.api_key,
            base_url=self.base_url,
            # timeout=60.0,  # 增加超时时间
            max_retries=0  # 禁用内置重试，使用自定义重试
        )

    @retry(
        stop=stop_after_attempt(5),  # 最多重试 5 次
        wait=wait_exponential(multiplier=1, min=2, max=60),  # 指数退避：2s, 4s, 8s, 16s, 32s
        retry=retry_if_exception_type((RateLimitError, APIConnectionError, APIError)),
        before_sleep=before_sleep_log(logger, logging.WARNING)
    )
    async def call_vlm(self, messages, **kwargs):
        """
        Async wrapper for VLM API calls with retry
        """
        try:
            response = await self.client.chat.completions.create(
                model=self.model,
                messages=messages,
                temperature=0.01,
                max_tokens=4000,
                **kwargs
            )
            return response.choices[0].message.content
        except RateLimitError as e:
            logger.warning(f"Rate limit hit, will retry: {e}")
            raise  # 让 tenacity 处理重试
        except (APIConnectionError, APIError) as e:
            logger.warning(f"API error, will retry: {e}")
            raise  # 让 tenacity 处理重试
        except Exception as e:
            logger.error(f"API Call Failed (no retry): {e}")
            raise  # 其他错误不重试
