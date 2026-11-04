# predict.py
import argparse
import json
import time
from pathlib import Path
from typing import List, Dict
from rag_system.retriever import HybridRetriever
from rag_system.reranker import Reranker
from rag_system.utils import get_adjacent_blocks, format_context, count_tokens
import requests


class RAGPredictor:
    def __init__(self, index_dir: str, config_path: str):
        with open(config_path, 'r', encoding='utf-8') as f:
            self.config = json.load(f)

        # Загружаем ретривер
        self.retriever = HybridRetriever(index_dir)
        self.retriever.load_index()

        # Загружаем embedding модель для ретривера
        embedding_model_name = self.config["retrieval"]["embedding_model"]
        from sentence_transformers import SentenceTransformer
        self.retriever.embedding_model = SentenceTransformer(embedding_model_name)

        # Инициализируем реранкер
        reranker_model = self.config["retrieval"]["reranker_model"]
        self.reranker = Reranker(model_name=reranker_model)

        # Параметры из конфига
        self.retrieval_config = self.config["retrieval"]
        self.limits = self.config["limits"]
        self.gen_params = self.config["generation_params"]
        self.access_config = self.config["access"]

    def call_llm(self, messages: List[Dict], question_id: str) -> Dict:
        """
        Вызывает LLM через OpenAI-совместимый API
        Возвращает ответ и статистику по токенам
        """
        headers = {
            "Content-Type": "application/json",
            "Authorization": f"Bearer {self.access_config['api_key']}",
            "X-Question-ID": question_id,
            "X-Submission-ID": "SUBMISSION_ID"  # Замените на реальный ID
        }

        payload = {
            "model": self.access_config["model"],
            "messages": messages,
            "temperature": self.gen_params.get("temperature", 0.1),
            "top_p": self.gen_params.get("top_p", 0.9),
            "max_tokens": self.gen_params.get("max_tokens", 1024),
        }

        response = requests.post(
            f"{self.access_config['base_url']}/chat/completions",
            headers=headers,
            json=payload,
            timeout=self.limits.get("timeout_seconds", 60)
        )

        if response.status_code != 200:
            raise Exception(f"LLM API error: {response.status_code} - {response.text}")

        result = response.json()

        # Извлекаем статистику токенов
        usage = result.get("usage", {})
        input_tokens = usage.get("prompt_tokens", 0)
        output_tokens = usage.get("completion_tokens", 0)

        return {
            "content": result["choices"][0]["message"]["content"],
            "input_tokens": input_tokens,
            "output_tokens": output_tokens
        }

    def retrieve_context(self, query: str, question_id: str) -> List[Dict]:
        """
        Извлекает релевантный контекст для запроса
        """
        # Hybrid search
        hybrid_results = self.retriever.hybrid_search(
            query,
            top_k_bm25=self.retrieval_config.get("top_k_bm25", 20),
            top_k_dense=self.retrieval_config.get("top_k_dense", 20),
            top_k_final=20
        )

        # Получаем блоки
        candidate_blocks = [self.retriever.get_block_by_idx(idx) for idx, _ in hybrid_results]

        # Расширяем контекст соседними блоками
        if self.retrieval_config.get("use_adjacent_blocks", True):
            window = self.retrieval_config.get("adjacent_window", 1)
            expanded_blocks = []
            seen_ids = set()

            for block in candidate_blocks[:5]:  # Берем топ-5 для расширения
                adjacent = get_adjacent_blocks(
                    block['block_id'],
                    self.retriever.block_id_to_idx,
                    self.retriever.corpus_blocks,
                    window=window
                )
                for adj_block in adjacent:
                    if adj_block['block_id'] not in seen_ids:
                        expanded_blocks.append(adj_block)
                        seen_ids.add(adj_block['block_id'])

            candidate_blocks = expanded_blocks

        # Реранкинг
        top_k_rerank = self.retrieval_config.get("top_k_rerank", 3)
        reranked = self.reranker.rerank(query, candidate_blocks, top_k=top_k_rerank)

        return [block for block, _ in reranked]

    def generate_answer(self, question: Dict, context_blocks: List[Dict], question_id: str) -> Dict:
        """
        Генерирует ответ на вопрос с использованием контекста
        """
        # Форматируем контекст
        max_context_tokens = 2500  # Оставляем место для question + answer
        context = format_context(context_blocks, max_tokens=max_context_tokens)

        # Формируем промпт
        question_text = question["question"]
        options_text = "\n".join([f"- {opt['id']}. {opt['text']}" for opt in question["options"]])

        system_prompt = """You are a precise mathematical QA assistant. You must answer ONLY based on the provided context.

Rules:
1. Analyze the context carefully
2. Select ALL correct options (can be multiple)
3. Provide brief rationale (max 1200 chars)
4. Cite relevant block_ids (max 4)
5. If context is insufficient, set abstain=true

Output JSON format:
{
  "answer_ids": ["A", "C"],
  "abstain": false,
  "rationale": "Brief explanation",
  "citations": ["b_0042", "b_0107"]
}"""

        user_prompt = f"""Context:
{context}

Question:
{question_text}

Options:
{options_text}

Provide your answer in JSON format."""

        messages = [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt}
        ]

        start_time = time.time()
        result = self.call_llm(messages, question_id)
        elapsed_ms = int((time.time() - start_time) * 1000)

        # Парсим JSON ответ
        try:
            # Извлекаем JSON из ответа (может быть в markdown блоке)
            content = result["content"]
            if "```json" in content:
                content = content.split("```json")[1].split("```")[0].strip()
            elif "```" in content:
                content = content.split("```")[1].split("```")[0].strip()

            answer = json.loads(content)

            # Валидация
            if not isinstance(answer.get("answer_ids"), list):
                answer["answer_ids"] = []
            if not isinstance(answer.get("citations"), list):
                answer["citations"] = []
            if not isinstance(answer.get("abstain"), bool):
                answer["abstain"] = False

            # Ограничиваем citations до 4
            answer["citations"] = answer["citations"][:4]

        except Exception as e:
            # Если парсинг не удался, возвращаем пустой ответ
            answer = {
                "answer_ids": [],
                "abstain": True,
                "rationale": f"Failed to parse LLM response: {str(e)}",
                "citations": []
            }

        return {
            "answer": answer,
            "input_tokens": result["input_tokens"],
            "output_tokens": result["output_tokens"],
            "elapsed_ms": elapsed_ms,
            "retrieved_block_ids": [b["block_id"] for b in context_blocks]
        }

    def predict_question(self, question: Dict) -> Dict:
        """
        Делает предсказание для одного вопроса
        """
        question_id = question["id"]

        # Извлекаем контекст
        context_blocks = self.retrieve_context(question["question"], question_id)

        # Генерируем ответ
        result = self.generate_answer(question, context_blocks, question_id)

        # Формируем выходной формат
        output = {
            "id": question_id,
            "answer_ids": result["answer"]["answer_ids"],
            "abstain": result["answer"]["abstain"],
            "rationale": result["answer"]["rationale"],
            "citations": result["answer"]["citations"]
        }

        # Формируем trace
        trace = {
            "id": question_id,
            "llm_calls": 1,
            "input_tokens": result["input_tokens"],
            "output_tokens": result["output_tokens"],
            "elapsed_ms": result["elapsed_ms"],
            "retrieved_block_ids": result["retrieved_block_ids"]
        }

        return output, trace

    def predict_all(self, questions_path: str, output_path: str, trace_path: str):
        """
        Делает предсказания для всех вопросов
        """
        # Загружаем вопросы
        questions = []
        with open(questions_path, 'r', encoding='utf-8') as f:
            for line in f:
                questions.append(json.loads(line.strip()))

        print(f"Processing {len(questions)} questions...")

        # Открываем файлы для записи
        with open(output_path, 'w', encoding='utf-8') as f_out, \
                open(trace_path, 'w', encoding='utf-8') as f_trace:

            for i, question in enumerate(questions, 1):
                print(f"Processing question {i}/{len(questions)}: {question['id']}")

                try:
                    output, trace = self.predict_question(question)

                    # Записываем результаты
                    f_out.write(json.dumps(output, ensure_ascii=False) + "\n")
                    f_out.flush()

                    f_trace.write(json.dumps(trace, ensure_ascii=False) + "\n")
                    f_trace.flush()

                except Exception as e:
                    print(f"Error processing {question['id']}: {str(e)}")
                    # Записываем пустой ответ с ошибкой
                    error_output = {
                        "id": question["id"],
                        "answer_ids": [],
                        "abstain": True,
                        "rationale": f"Error: {str(e)}",
                        "citations": []
                    }
                    f_out.write(json.dumps(error_output, ensure_ascii=False) + "\n")
                    f_out.flush()

        print(f"Predictions saved to {output_path}")
        print(f"Trace saved to {trace_path}")


def main():
    parser = argparse.ArgumentParser(description="Run RAG predictions")
    parser.add_argument("--index", required=True, help="Path to index directory")
    parser.add_argument("--questions", required=True, help="Path to questions.jsonl")
    parser.add_argument("--output", required=True, help="Path to output answers.jsonl")
    parser.add_argument("--trace", required=True, help="Path to trace.jsonl")
    parser.add_argument("--config", default="runtime.json", help="Path to runtime config")
    args = parser.parse_args()

    # Создаем предиктор
    predictor = RAGPredictor(args.index, args.config)

    # Запускаем предсказания
    predictor.predict_all(args.questions, args.output, args.trace)


if __name__ == "__main__":
    main()