# rag_system/utils.py
from typing import List, Dict
import tiktoken
import re
import json

def safe_json_parse(content: str):
    """
    Безопасно парсит JSON от LLM, исправляя невалидные escape-последовательности
    (часто возникают из-за LaTeX формул в тексте)
    """
    # Первая попытка — как есть
    try:
        return json.loads(content)
    except json.JSONDecodeError as e:
        if "escape" not in str(e).lower():
            raise

        # Исправляем одинарные обратные слеши, не являющиеся валидными escape
        # Валидные: \", \\, \/, \b, \f, \n, \r, \t, \uXXXX
        fixed = re.sub(r'\\(?!["\\/bfnrtu])', r'\\\\', content)

        try:
            return json.loads(fixed)
        except json.JSONDecodeError:
            # Fallback: пробуем убрать все слеши (крайний случай)
            fixed_no_slash = content.replace('\\', '')
            return json.loads(fixed_no_slash)

def count_tokens(text: str, model_name: str = "gpt-3.5-turbo") -> int:
    """
    Подсчитывает количество токенов в тексте
    Используем tiktoken как аппроксимацию (Qwen токенизатор похож)
    """
    try:
        encoding = tiktoken.encoding_for_model(model_name)
        return len(encoding.encode(text))
    except:
        # Fallback: примерная оценка 1 токен ≈ 4 символа для русского
        return len(text) // 4


def get_adjacent_blocks(block_id: str,
                        block_id_to_idx: Dict[str, int],
                        corpus_blocks: List[Dict],
                        window: int = 1) -> List[Dict]:
    """
    Получает соседние блоки для расширения контекста
    window=1 вернет [prev, current, next]
    """
    if block_id not in block_id_to_idx:
        return []

    idx = block_id_to_idx[block_id]
    result = []

    # Добавляем блоки в окне
    for i in range(max(0, idx - window), min(len(corpus_blocks), idx + window + 1)):
        result.append(corpus_blocks[i])

    return result


def format_context(blocks: List[Dict], max_tokens: int = 2000) -> str:
    """
    Форматирует блоки в строку контекста для LLM
    Обрезает если превышает max_tokens
    """
    context_parts = []
    total_tokens = 0

    for block in blocks:
        block_text = f"[block_id: {block['block_id']}, page: {block['page']}]\n{block['text']}\n\n"
        block_tokens = count_tokens(block_text)

        if total_tokens + block_tokens > max_tokens:
            break

        context_parts.append(block_text)
        total_tokens += block_tokens

    return "".join(context_parts)
