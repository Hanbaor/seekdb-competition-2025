import os
import asyncio
from openai import AsyncOpenAI
import logging

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
            base_url=self.base_url
        )

    async def call_vlm(self, messages, **kwargs):
        """
        Async wrapper for VLM API calls
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
        except Exception as e:
            logger.error(f"API Call Failed: {e}")
            raise
