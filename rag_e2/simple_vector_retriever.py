"""
简化版向量检索器：跳过E2GraphRAG的图和树构建
只使用：VLM解析 → 分块 → FAISS索引 → 向量检索
"""
import faiss
import numpy as np
from typing import List, Dict
import pickle
import os

class SimpleVectorRetriever:
    """纯FAISS向量检索，不构建图和树"""
    
    def __init__(self, doc_name: str, chunks: List[str], embedder):
        """
        Args:
            doc_name: 文档名称
            chunks: 已经分好的文本块列表
            embedder: embedding模型
        """
        self.doc_name = doc_name
        self.chunks = chunks
        self.embedder = embedder
        
        # 构建FAISS索引
        self._build_index()
    
    def _build_index(self):
        """构建FAISS向量索引"""
        # 1. 为所有chunks生成embeddings (批量调用以提高效率并获得正确的shape)
        embeddings = self.embedder.encode(self.chunks)
        
        # 确保是float32且shape正确
        embeddings = np.array(embeddings).astype('float32')
        if len(embeddings.shape) == 3:  # 如果意外出现了三维 (N, 1, D)
            embeddings = embeddings.squeeze(1)
        
        # 2. 创建FAISS索引
        dim = embeddings.shape[1]
        self.index = faiss.IndexFlatIP(dim)  # 使用内积（余弦相似度）
        
        # 3. 归一化embeddings（使内积等价于余弦相似度）
        faiss.normalize_L2(embeddings)
        
        # 4. 添加到索引
        self.index.add(embeddings)
        
        print(f"  Built FAISS index: {len(self.chunks)} chunks, dim={dim}")
    
    def query(self, query: str, top_k: int = 6, query_emb=None, **kwargs) -> Dict:
        """
        检索最相关的chunks
        
        Args:
            query: 查询文本
            top_k: 返回top k个最相关chunks
            query_emb: 可选，预计算好的查询向量 (np.ndarray)
            
        Returns:
            包含chunks和debug信息的字典
        """
        # 1. 生成query embedding (如果未提供)
        if query_emb is None:
            query_emb = self.embedder.encode(query).reshape(1, -1).astype('float32')
            faiss.normalize_L2(query_emb)
        
        # 2. FAISS检索
        scores, indices = self.index.search(query_emb, min(top_k, len(self.chunks)))
        
        # 3. 获取对应的chunks
        retrieved_chunks = []
        for idx in indices[0]:
            if 0 <= idx < len(self.chunks):
                retrieved_chunks.append(self.chunks[idx])
        
        # 4. 拼接结果
        chunks_text = "\n\n".join(retrieved_chunks)
        
        # 5. 构建返回结果（兼容E2GraphRAG格式）
        result = {
            "chunks": chunks_text,
            "retrieval_type": "Simple Vector Retrieval",
            "entities": [],  # 不提取实体
            "chunk_ids": {"": [f"chunk_{i}" for i in indices[0]]},
            "scores": scores[0].tolist()
        }
        
        return result
    
    @staticmethod
    def from_vlm_output(doc_name: str, vlm_text: str, embedder, chunk_size: int = 512):
        """
        从VLM解析的文本创建检索器
        
        Args:
            doc_name: 文档名称
            vlm_text: VLM解析的完整文本
            embedder: embedding模型
            chunk_size: 每个chunk的token数量
            
        Returns:
            SimpleVectorRetriever实例
        """
        # 简单分块：按段落分割
        # 实际应该用tiktoken按token数分块，这里简化处理
        paragraphs = vlm_text.split('\n\n')
        
        # 合并段落直到达到chunk_size
        chunks = []
        current_chunk = []
        current_length = 0
        
        for para in paragraphs:
            para = para.strip()
            if not para:
                continue
            
            para_length = len(para)
            
            if current_length + para_length > chunk_size * 4 and current_chunk:  # 简化：假设1 token ≈ 4 chars
                chunks.append('\n\n'.join(current_chunk))
                current_chunk = [para]
                current_length = para_length
            else:
                current_chunk.append(para)
                current_length += para_length
        
        if current_chunk:
            chunks.append('\n\n'.join(current_chunk))
        
        print(f"  Split into {len(chunks)} chunks")
        
        return SimpleVectorRetriever(doc_name, chunks, embedder)
    
    def save(self, save_path: str):
        """保存索引"""
        os.makedirs(os.path.dirname(save_path), exist_ok=True)
        
        data = {
            'doc_name': self.doc_name,
            'chunks': self.chunks,
        }
        
        with open(save_path + '.pkl', 'wb') as f:
            pickle.dump(data, f)
        
        faiss.write_index(self.index, save_path + '.faiss')
    
    @staticmethod
    def load(load_path: str, embedder):
        """加载索引"""
        with open(load_path + '.pkl', 'rb') as f:
            data = pickle.load(f)
        
        retriever = SimpleVectorRetriever.__new__(SimpleVectorRetriever)
        retriever.doc_name = data['doc_name']
        retriever.chunks = data['chunks']
        retriever.embedder = embedder
        retriever.index = faiss.read_index(load_path + '.faiss')
        
        return retriever
