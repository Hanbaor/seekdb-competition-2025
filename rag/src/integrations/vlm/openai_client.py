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
    model: str = "qwen3-vl-flash",
    wait_seconds: float = 2.0,
) -> str:
    """
    Call VLM to process text and images, retry indefinitely on transient errors.
    
    Args:
        prompt: Text prompt
        images: Image path(s) or base64 encoded string(s), optional
        model: VLM model name
        wait_seconds: Wait time between retries
    
    Returns:
        Generated text response
    """
    # Store original image paths for compression retry on 502 errors
    src_image_paths = []
    if images:
        img_list = [images] if isinstance(images, str) else images
        src_image_paths = [p for p in img_list if os.path.isfile(p)]
    
    # Convert images to base64
    base64_images, temp_files = process_images_to_base64(
        images if images else [], compress_for_retry=False
    )
    
    # Build message content
    msg_content = [{"type": "text", "text": prompt}]
    for b64 in base64_images:
        msg_content.append({
            "type": "image_url",
            "image_url": {"url": f"data:image/png;base64,{b64}"}
        })
    
    request_messages = [{"role": "user", "content": msg_content}]
    already_compressed = False
    max_retries = 30
    
    for retry_count in range(1, max_retries + 1):
        try:
            resp = vlm_client.chat.completions.create(
                model=model,
                messages=request_messages,
                temperature=0.2,
            )
            
            text = resp.choices[0].message.content if resp.choices else None
            if not text or not text.strip():
                raise ValueError("Empty response from VLM")
            
            cleanup_temp_files(temp_files)
            return text
            
        except (RateLimitError, APIConnectionError) as err:
            if retry_count >= max_retries:
                logger.error(f"[VLM] Failed after {max_retries} retries: {err}")
                cleanup_temp_files(temp_files)
                raise
            logger.warning(f"[VLM retry {retry_count}/{max_retries}] {err.__class__.__name__}, retrying in {wait_seconds}s")
            time.sleep(wait_seconds)
            
        except APIError as err:
            err_msg = str(err).lower()
            http_code = getattr(err, "status_code", None)
            
            # Image format errors are not retryable
            bad_format_hints = ["unknown format", "unsupported format", "invalid format", "image format", "failed to process"]
            if any(hint in err_msg for hint in bad_format_hints):
                logger.error(f"Image format error: {err}")
                cleanup_temp_files(temp_files)
                raise
            
            # Try compressing images on 502 errors
            is_gateway_err = (http_code == 502 or "502" in err_msg or "bad gateway" in err_msg)
            if is_gateway_err and src_image_paths and not already_compressed:
                logger.warning("502 error detected, attempting to compress images...")
                try:
                    cleanup_temp_files(temp_files)
                    base64_images, temp_files = process_images_to_base64(src_image_paths, compress_for_retry=True)
                    msg_content = [{"type": "text", "text": prompt}]
                    for b64 in base64_images:
                        msg_content.append({
                            "type": "image_url", 
                            "image_url": {"url": f"data:image/png;base64,{b64}"}
                        })
                    request_messages = [{"role": "user", "content": msg_content}]
                    already_compressed = True
                    logger.info("Images compressed, retrying...")
                    continue
                except Exception as compress_err:
                    logger.error(f"Failed to compress images: {compress_err}")
            
            # 5xx server errors are retryable
            if http_code and http_code >= 429:
                if retry_count >= max_retries:
                    logger.error(f"[VLM] Failed after {max_retries} retries: {err}")
                    cleanup_temp_files(temp_files)
                    raise
                logger.warning(f"[VLM retry {retry_count}/{max_retries}] Server error {http_code}, retrying in {wait_seconds}s")
                time.sleep(wait_seconds)
            else:
                cleanup_temp_files(temp_files)
                raise
                
        except Exception:
            cleanup_temp_files(temp_files)
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
