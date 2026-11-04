# rag_system/reranker.py
from typing import List, Tuple, Dict
from sentence_transformers import CrossEncoder


class Reranker:
    def __init__(self, model_name: str = "BAAI/bge-reranker-v2-m3"):
        self.model_name = model_name
        self.model = None

    def load_model(self):
        """Загружает модель реранкера"""
        if self.model is None:
            print(f"Loading reranker model: {self.model_name}")
            self.model = CrossEncoder(self.model_name)

    def rerank(self,
               query: str,
               blocks: List[Dict],
               top_k: int = 3) -> List[Tuple[Dict, float]]:
        """
        Реранкает блоки по релевантности к запросу
        Возвращает список (блок, скор)
        """
        if self.model is None:
            self.load_model()

        # Создаем пары (query, text)
        pairs = [[query, block['text']] for block in blocks]

        # Получаем скоры от CrossEncoder
        scores = self.model.predict(pairs)

        # Сортируем по скору (убывание)
        block_scores = list(zip(blocks, scores))
        block_scores.sort(key=lambda x: x[1], reverse=True)

        return block_scores[:top_k]
