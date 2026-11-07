# rag_system/indexer.py
import json
import pickle
import numpy as np
from pathlib import Path
from typing import List, Dict
from rank_bm25 import BM25Okapi
from sentence_transformers import SentenceTransformer
import re


class CorpusIndexer:
    def __init__(self, embedding_model_name: str = "BAAI/bge-m3"):
        self.embedding_model_name = embedding_model_name
        self.embedding_model = None
        self.bm25 = None
        self.corpus_blocks = []  # Список всех блоков
        self.block_id_to_idx = {}  # Маппинг block_id -> индекс
        self.dense_vectors = None

    def load_corpus(self, corpus_path: str):
        """Загружает корпус из JSONL файла"""
        self.corpus_blocks = []
        with open(corpus_path, 'r', encoding='utf-8') as f:
            for line in f:
                block = json.loads(line.strip())
                self.corpus_blocks.append(block)

        self.block_id_to_idx = {b['block_id']: i for i, b in enumerate(self.corpus_blocks)}
        print(f"Loaded {len(self.corpus_blocks)} blocks")

    def _get_texts_for_indexing(self) -> List[str]:
        """Подготавливает тексты для индексации, отсеивая мусор"""
        texts = []
        valid_blocks = []
        min_text_length = 80  # Минимум символов для индексации

        for block in self.corpus_blocks:
            text = block['text'].strip()

            # Фильтр 1: слишком короткие блоки
            if len(text) < min_text_length:
                continue

            # Фильтр 2: блоки, состоящие только из пунктуации/цифр
            if len(re.sub(r'[\s\d\W_]', '', text)) < 15:
                continue

            # Фильтр 3: известные "хвосты"
            trash_patterns = [
                "и лемма доказана", "лемма доказана", "теорема доказана",
                "и теорема доказана", "что и требовалось", "ч.т.д."
            ]
            if text.lower() in trash_patterns or text.lower().endswith("доказана."):
                continue

            section_text = f"{block.get('section', '')} {text}".strip()
            texts.append(section_text)
            valid_blocks.append(block)

        # Перезаписываем corpus_blocks только валидными блоками
        self.corpus_blocks = valid_blocks
        self.block_id_to_idx = {b['block_id']: i for i, b in enumerate(self.corpus_blocks)}
        print(f"Filtered out {len(self.corpus_blocks) - len(valid_blocks)} short blocks")

        return texts

    def build_bm25_index(self):
        """Строит BM25 индекс"""
        texts = self._get_texts_for_indexing()
        # Токенизация для русского языка (простая по пробелам + пунктуация)
        tokenized = [text.lower().split() for text in texts]
        self.bm25 = BM25Okapi(tokenized)
        print("BM25 index built")

    def build_dense_index(self, batch_size: int = 16):
        """Строит Dense индекс с использованием embedding модели"""
        if self.embedding_model is None:
            print(f"Loading embedding model: {self.embedding_model_name}")
            self.embedding_model = SentenceTransformer(self.embedding_model_name)

        texts = self._get_texts_for_indexing()

        # Генерируем эмбеддинги батчами
        print(f"Generating embeddings for {len(texts)} blocks...")
        embeddings = self.embedding_model.encode(
            texts,
            batch_size=batch_size,
            show_progress_bar=True,
            convert_to_numpy=True,
            normalize_embeddings=True  # Для косинусного сходства
        )

        self.dense_vectors = embeddings
        print(f"Dense index built: shape {self.dense_vectors.shape}")

    def save_index(self, output_dir: str):
        """Сохраняет все индексы в директорию"""
        output_path = Path(output_dir)
        output_path.mkdir(parents=True, exist_ok=True)

        # Сохраняем корпус
        with open(output_path / "corpus_blocks.pkl", "wb") as f:
            pickle.dump(self.corpus_blocks, f)

        # Сохраняем BM25
        if self.bm25 is not None:
            with open(output_path / "bm25_index.pkl", "wb") as f:
                pickle.dump(self.bm25, f)

        # Сохраняем Dense векторы
        if self.dense_vectors is not None:
            np.save(output_path / "dense_vectors.npy", self.dense_vectors)

        # Сохраняем метаданные
        metadata = {
            "embedding_model": self.embedding_model_name,
            "num_blocks": len(self.corpus_blocks),
            "block_id_to_idx": self.block_id_to_idx
        }
        with open(output_path / "metadata.json", "w", encoding="utf-8") as f:
            json.dump(metadata, f, ensure_ascii=False, indent=2)

        print(f"Index saved to {output_dir}")



