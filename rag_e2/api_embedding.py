"""
API-based embedding wrapper compatible with SentenceTransformer interface.
"""
import os
import logging
import asyncio
from typing import List, Union
import numpy as np
from openai import OpenAI

logger = logging.getLogger(__name__)


class APIEmbedding:
    """Wrapper for API-based embedding that mimics SentenceTransformer interface."""
    
    def __init__(self, model_name: str = "text-embedding-v4", embedding_dim: int = 1024, batch_size: int = 6):
        """
        Initialize API embedding client.
        
        Args:
            model_name: Name of the embedding model
            embedding_dim: Dimension of the embedding vectors
            batch_size: Maximum batch size for API calls
        """
        self.model_name = model_name
        self.embedding_dim = embedding_dim
        self.batch_size = batch_size
        
        # Initialize OpenAI client
        api_key = os.getenv("OPENAI_API_KEY")
        base_url = os.getenv("OPENAI_BASE_URL", "https://dashscope.aliyuncs.com/compatible-mode/v1")
        
        if not api_key:
            raise ValueError("OPENAI_API_KEY environment variable is required for API embedding")
        
        self.client = OpenAI(api_key=api_key, base_url=base_url)
        logger.info(f"Initialized APIEmbedding with model={model_name}, dim={embedding_dim}")
    
    def encode(
        self, 
        sentences: Union[str, List[str]], 
        batch_size: int = None,
        convert_to_numpy: bool = True,
        show_progress_bar: bool = False,
        **kwargs
    ) -> Union[np.ndarray, List[List[float]]]:
        """
        Encode sentences into embeddings using API.
        
        Args:
            sentences: Single sentence string or list of sentences
            batch_size: Override default batch size
            convert_to_numpy: Whether to convert result to numpy array
            show_progress_bar: Ignored (for compatibility)
            **kwargs: Additional arguments (ignored)
            
        Returns:
            Embeddings as numpy array or list
        """
        # Handle single string
        if isinstance(sentences, str):
            sentences = [sentences]
        
        batch_size = batch_size or self.batch_size
        all_embeddings = []
        
        # Process in batches
        for i in range(0, len(sentences), batch_size):
            batch = sentences[i:i+batch_size]
            try:
                response = self.client.embeddings.create(
                    model=self.model_name,
                    input=batch
                )
                batch_embeddings = [item.embedding for item in response.data]
                all_embeddings.extend(batch_embeddings)
            except Exception as e:
                logger.error(f"API embedding failed for batch {i//batch_size}: {e}")
                # Return zero vectors as fallback
                all_embeddings.extend([[0.0] * self.embedding_dim] * len(batch))
        
        if convert_to_numpy:
            return np.array(all_embeddings, dtype=np.float32)
        return all_embeddings
    
    def get_sentence_embedding_dimension(self) -> int:
        """Return embedding dimension."""
        return self.embedding_dim
