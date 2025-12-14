import logging
import os
import time
from typing import List, Optional, Union

import dotenv
from openai import APIConnectionError, APIError, OpenAI, RateLimitError

from .image_utils import cleanup_temp_files, process_images_to_base64

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
vlm_client = OpenAI(
    api_key=OPENAI_API_KEY,
    base_url=OPENAI_BASE_URL,
)


def generate_response(
    prompt: str,
    images: Optional[Union[str, List[str]]] = None,
    model: str = "qwen3-vl-plus",
    retry_interval: float = 2.0,  # pause duration between retries
    temperature: float = 0.1,
) -> str:
    """
    Query vision-language model with text and optional images.
    Automatically handles failures by retrying until success.

    Args:
        prompt: Text instruction for the model
        images: Image file paths or base64 strings (single or list)
        model: VLM model identifier
        retry_interval: Seconds to pause before retry attempt

    Returns:
        Model's text output as string
    """

    # Store original image paths for potential compression on errors
    original_image_paths = []
    if images:
        if isinstance(images, str):
            images = [images]
        original_image_paths = [img if os.path.isfile(img) else None for img in images]

    # Process images - convert file paths to base64 if needed
    processed_images, temp_files_to_cleanup = process_images_to_base64(
        images if images else [], compress_for_retry=False
    )

    # Build messages in OpenAI format
    # OpenAI format: content is a list of content blocks
    content_blocks = [{"type": "text", "text": prompt}]

    # Add images to content blocks
    for img_base64 in processed_images:
        # OpenAI format: data:image/{format};base64,{base64_string}
        # We'll use PNG as default format since we normalize to PNG
        image_url = f"data:image/png;base64,{img_base64}"
        content_blocks.append({"type": "image_url", "image_url": {"url": image_url}})

    messages = [{"role": "user", "content": content_blocks}]

    # Track if we've already compressed images for retry
    images_compressed = False
    attempt = 0

    while True:
        attempt += 1
        try:
            response = vlm_client.chat.completions.create(
                model=model,
                messages=messages,
                temperature=0.1,
            )

            if not response.choices or not response.choices[0].message.content:
                raise ValueError("Empty response from API")

            content = response.choices[0].message.content
            if not content or len(content.strip()) == 0:
                raise ValueError("Empty response content from API")

            # Clean up temporary files before returning
            cleanup_temp_files(temp_files_to_cleanup)

            return content

        except (APIConnectionError, RateLimitError) as e:
            # 网络错误、限流：无限重试
            logger.warning(f"VLM error (attempt {attempt}): {type(e).__name__}: {str(e)[:100]}. Retrying in {retry_interval}s...")
            time.sleep(retry_interval)
        except APIError as e:
            error_str = str(e).lower()
            
            # Handle 502 errors - try compressing images
            is_502_error = "502" in error_str or "bad gateway" in error_str
            if is_502_error and original_image_paths and not images_compressed:
                logger.warning("502 error detected. Attempting to compress images...")
                try:
                    cleanup_temp_files(temp_files_to_cleanup)
                    processed_images, temp_files_to_cleanup = process_images_to_base64(
                        [img for img in original_image_paths if img is not None],
                        compress_for_retry=True,
                    )
                    content_blocks = [{"type": "text", "text": prompt}]
                    for img_base64 in processed_images:
                        image_url = f"data:image/png;base64,{img_base64}"
                        content_blocks.append(
                            {"type": "image_url", "image_url": {"url": image_url}}
                        )
                    messages = [{"role": "user", "content": content_blocks}]
                    images_compressed = True
                    logger.info("Images compressed, retrying...")
                    continue
                except Exception as compress_error:
                    logger.error(f"Failed to compress images: {compress_error}")
            
            # API 错误：5xx 服务端错误重试，4xx 客户端错误直接抛出
            if hasattr(e, 'status_code') and e.status_code >= 500:
                logger.warning(f"VLM server error (attempt {attempt}): {e}. Retrying in {retry_interval}s...")
                time.sleep(retry_interval)
            else:
                logger.error(f"VLM API error (non-retryable): {e}")
                cleanup_temp_files(temp_files_to_cleanup)
                raise
        except Exception as e:
            # 其他未知错误：直接抛出
            logger.error(f"VLM unexpected error: {type(e).__name__}: {e}")
            cleanup_temp_files(temp_files_to_cleanup)
            raise


if __name__ == "__main__":
    # Test with text only
    result = generate_response("What is a vision language model?")
    print(f"Text-only response: {result}")

    # Test with image (uncomment and provide image path)
    # result = generate_response(
    #     "Describe this image in detail.",
    #     images="path/to/image.jpg"
    # )
    # print(f"Image response: {result}")
