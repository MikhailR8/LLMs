# predict.py
import argparse
import json
import time
from typing import List, Dict, Tuple

from markdown_it.utils import OptionsDict

from rag_system.retriever import HybridRetriever
from rag_system.reranker import Reranker
from rag_system.utils import get_adjacent_blocks, format_context, safe_json_parse
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

    def call_llm(self, messages: List[Dict], question_id: str, reasoning_effort: str | None = None) -> Dict:
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
            "temperature": self.gen_params.get("temperature"),
            "top_p": self.gen_params.get("top_p"),
            "max_completion_tokens": self.gen_params.get("max_completion_tokens"),
            "reasoning_effort": self.gen_params.get("reasoning_effort") if not reasoning_effort else reasoning_effort,
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

        return {
            "content": result["choices"][0]["message"]["content"],
            "input_tokens": usage.get("prompt_tokens", 0),
            "output_tokens": usage.get("completion_tokens", 0)
        }

    def retrieve_context(self, expanded_queries: List[str]) -> List[Dict]:
        """
        Извлекает релевантный контекст используя несколько запросов
        """
        all_candidates = []
        seen_ids = set()

        # Поиск по каждому запросу
        for i, q in enumerate(expanded_queries):
            if i == 0:
                hybrid_results = self.retriever.hybrid_search(
                    q,
                    top_k_bm25=self.retrieval_config.get("top_k_bm25", 10),
                    top_k_dense=self.retrieval_config.get("top_k_dense", 10),
                    top_k_final=10
                )
            else:
                hybrid_results = self.retriever.hybrid_search(
                    q,
                    top_k_bm25=self.retrieval_config.get("top_k_bm25", 3),
                    top_k_dense=self.retrieval_config.get("top_k_dense", 3),
                    top_k_final=3
                )

            for idx, score in hybrid_results:
                block = self.retriever.get_block_by_idx(idx)
                if block['block_id'] not in seen_ids:
                    all_candidates.append((block, score))
                    seen_ids.add(block['block_id'])

        # # Сортируем по скору (блоки с высоким скором от любого запроса)
        # all_candidates.sort(key=lambda x: x[1], reverse=True)
        candidate_blocks = [block for block, _ in all_candidates]  # уникальные

        # Расширяем контекст соседними блоками
        if self.retrieval_config.get("use_adjacent_blocks", True):
            window = self.retrieval_config.get("adjacent_window", 1)
            expanded_blocks = []
            seen_ids_expanded = set()

            for block in candidate_blocks:  # Берем топ-8 для расширения
                adjacent = get_adjacent_blocks(
                    block['block_id'],
                    self.retriever.block_id_to_idx,
                    self.retriever.corpus_blocks,
                    window=window
                )
                for adj_block in adjacent:
                    if adj_block['block_id'] not in seen_ids_expanded:
                        expanded_blocks.append(adj_block)
                        seen_ids_expanded.add(adj_block['block_id'])

            candidate_blocks = expanded_blocks
        # print([block['text'] for block in candidate_blocks])

        # Реранкинг (используем ПЕРВЫЙ запрос для реранкинга)
        top_k_rerank = self.retrieval_config.get("top_k_rerank", 5)
        reranked = self.reranker.rerank(expanded_queries[0], candidate_blocks, top_k=top_k_rerank)

        return [block for block, _ in reranked]

    def expand_query(self, question: Dict, question_id: str) -> List[str]:
        """
        Генерирует дополнительные поисковые запросы для математических задач
        """
        self._expand_tokens = {"input": 0, "output": 0}
        question_text = question["question"]
        options_text = "\n".join([f"- {opt['id']}. {opt['text']}" for opt in question["options"]])

        system_prompt = """You are a mathematical search query expert. Generate 1-2 additional search queries in Russian to find relevant formulas/theorems for solving this problem 
        ONLY if you think that problem need additional information, that doesn't contains EXPLICITLY in question.

    Focus on:
    - Mathematical methods needed (e.g., "формула Коши для производных", "теорема о вычетах")
    - Key mathematical objects (e.g., "интеграл по замкнутому контуру", "полюс порядка n")
    - Specific formulas referenced in options

    Return ONLY a JSON array of 1-2 strings (no more than 15 words), or word "NONE" in case no need for additional information."""

        user_prompt = f"""Question: {question_text}

    Options:
    {options_text}

    Generate search queries:"""

        messages = [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt}
        ]

        result = self.call_llm(messages, question_id, reasoning_effort='none')
        self._expand_tokens["input"] = result["input_tokens"]
        self._expand_tokens["output"] = result["output_tokens"]
        print(result['content'])

        try:
            content = result["content"]
            if "```json" in content:
                content = content.split("```json")[1].split("```")[0].strip()
            elif "```" in content:
                content = content.split("```")[1].split("```")[0].strip()

            queries = json.loads(content)
            if isinstance(queries, list):
                return [question_text] + queries[:2]  # Оригинальный + до 3 дополнительных
        except:
            return [question_text]  # Fallback: только оригинальный запрос


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

        system_prompt = \
"""Ты - помощник по вопросам теории функций комплексного переменного. Отвечай на вопросы, опираясь ТОЛЬКО на предоставленные данные.
Правила:
1. Проанализируй предоставленные сведения из учебника
2. Выбери ВСЕ правильные ответы (может быть несколько)
3. Предоставь короткое обоснование выбранного ответа на русском (rationale), max 40 слов
4. В citations необходимы номера релевантных блоков block_ids (max 4)
5. Если сведений недостаточно, ЯВНО скажи об этом в обосновании и установи "answer_ids": [], "abstain": true

Формат ответа (JSON):
{"rationale": "*Короткое пояснение*", "citations": ["b_0042", "b_0107"], "answer_ids": ["A", "C"], "abstain": false}"""

        user_prompt = \
f"""Вопрос: {question_text}
Сведения из учебника: 
{context}
Варианты ответа: 
{options_text}
Ответ в JSON формате: """

        messages = [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt}
        ]
        # print(user_prompt)

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

            answer = safe_json_parse(content)

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

    def predict_question(self, question: Dict) -> Tuple[Dict, Dict]:
        """
        Делает предсказание для одного вопроса с query expansion
        """
        question_id = question["id"]

        # Генерируем расширенные запросы (1-й вызов LLM)
        expanded_queries = self.expand_query(question, question_id)

        # Извлекаем контекст используя все запросы
        context_blocks = self.retrieve_context(question["question"])

        # Генерируем ответ (2-й вызов LLM)
        result = self.generate_answer(question, context_blocks, question_id)

        # Формируем выходной формат
        output = {
            "id": question_id,
            "answer_ids": result["answer"]["answer_ids"],
            "abstain": result["answer"]["abstain"],
            "rationale": result["answer"]["rationale"],
            "citations": result["answer"]["citations"]
        }

        # Формируем trace (суммируем токены от обоих вызовов)
        trace = {
            "id": question_id,
            "llm_calls": 2,
            "input_tokens": self._expand_tokens["input"] + result["input_tokens"],
            "output_tokens": self._expand_tokens["output"] + result["output_tokens"],
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