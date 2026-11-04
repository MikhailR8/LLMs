# rag_system/retriever.py
import json
import pickle
import numpy as np
from pathlib import Path
from typing import List, Dict, Tuple
from rank_bm25 import BM25Okapi
from sentence_transformers import SentenceTransformer
import heapq


class HybridRetriever:
    def __init__(self, index_dir: str):
        self.index_dir = Path(index_dir)
        self.corpus_blocks = []
        self.block_id_to_idx = {}
        self.bm25 = None
        self.dense_vectors = None
        self.embedding_model = None
        self.embedding_model_name = None

    def load_index(self):
        """Загружает все индексы из директории"""
        # Загружаем метаданные
        with open(self.index_dir / "metadata.json", "r", encoding="utf-8") as f:
            metadata = json.load(f)
            self.embedding_model_name = metadata["embedding_model"]
            self.block_id_to_idx = metadata["block_id_to_idx"]

        # Загружаем корпус
        with open(self.index_dir / "corpus_blocks.pkl", "rb") as f:
            self.corpus_blocks = pickle.load(f)

        # Загружаем BM25
        bm25_path = self.index_dir / "bm25_index.pkl"
        if bm25_path.exists():
            with open(bm25_path, "rb") as f:
                self.bm25 = pickle.load(f)

        # Загружаем Dense векторы
        dense_path = self.index_dir / "dense_vectors.npy"
        if dense_path.exists():
            self.dense_vectors = np.load(dense_path)

        print(f"Loaded index with {len(self.corpus_blocks)} blocks")

    def search_bm25(self, query: str, top_k: int = 20) -> List[Tuple[int, float]]:
        """Поиск по BM25"""
        if self.bm25 is None:
            return []

        tokenized_query = query.lower().split()
        scores = self.bm25.get_scores(tokenized_query)

        # Получаем топ-k индексов с наибольшими скорами
        top_indices = heapq.nlargest(top_k, range(len(scores)), key=lambda i: scores[i])
        return [(idx, scores[idx]) for idx in top_indices]

    def search_dense(self, query: str, top_k: int = 20) -> List[Tuple[int, float]]:
        """Поиск по Dense векторам"""
        if self.dense_vectors is None or self.embedding_model is None:
            return []

        # Получаем эмбеддинг запроса
        query_embedding = self.embedding_model.encode(
            [query],
            convert_to_numpy=True,
            normalize_embeddings=True
        )[0]

        # Косинусное сходство (векторы уже нормализованы)
        scores = np.dot(self.dense_vectors, query_embedding)

        # Получаем топ-k
        top_indices = heapq.nlargest(top_k, range(len(scores)), key=lambda i: scores[i])
        return [(idx, scores[idx]) for idx in top_indices]

    def reciprocal_rank_fusion(self,
                               bm25_results: List[Tuple[int, float]],
                               dense_results: List[Tuple[int, float]],
                               k: int = 60) -> List[Tuple[int, float]]:
        """
        Reciprocal Rank Fusion (RRF) для объединения результатов
        RRF score = sum(1 / (k + rank))
        """
        rrf_scores = {}

        # BM25 ранги
        for rank, (idx, _) in enumerate(bm25_results):
            if idx not in rrf_scores:
                rrf_scores[idx] = 0.0
            rrf_scores[idx] += 1.0 / (k + rank + 1)

        # Dense ранги
        for rank, (idx, _) in enumerate(dense_results):
            if idx not in rrf_scores:
                rrf_scores[idx] = 0.0
            rrf_scores[idx] += 1.0 / (k + rank + 1)

        # Сортируем по RRF score
        sorted_results = sorted(rrf_scores.items(), key=lambda x: x[1], reverse=True)
        return sorted_results

    def hybrid_search(self,
                      query: str,
                      top_k_bm25: int = 20,
                      top_k_dense: int = 20,
                      top_k_final: int = 20) -> List[Tuple[int, float]]:
        """
        Гибридный поиск: BM25 + Dense + RRF
        Возвращает список (индекс блока, гибридный скор)
        """
        bm25_results = self.search_bm25(query, top_k_bm25)
        dense_results = self.search_dense(query, top_k_dense)

        # Объединяем через RRF
        hybrid_results = self.reciprocal_rank_fusion(bm25_results, dense_results)

        return hybrid_results[:top_k_final]

    def get_block_by_idx(self, idx: int) -> Dict:
        """Получает блок по индексу"""
        return self.corpus_blocks[idx]

    def get_block_by_id(self, block_id: str) -> Dict:
        """Получает блок по block_id"""
        idx = self.block_id_to_idx.get(block_id)
        if idx is not None:
            return self.corpus_blocks[idx]
        return None
