import re
import json
import argparse


def parse_qa_markdown(md_path: str):
    with open(md_path, 'r', encoding='utf-8') as f:
        content = f.read()

    # Разбиваем текст на отдельные вопросы по заголовкам ## 1., ## 2. и т.д.
    # Регулярка ищет ## Число. Название и захватывает всё до следующего ## Число.
    question_blocks = re.split(r'(?=##\s*\d+\.)', content)

    questions = []
    gold = []

    for i, block in enumerate(question_blocks):
        if not block.strip().startswith('##'):
            continue

        # 1. Извлекаем ID (например, "1." -> "q_001")
        id_match = re.search(r'##\s*(\d+)\.', block)
        if not id_match: continue
        q_id = f"q_{int(id_match.group(1)):03d}"

        # 2. Извлекаем текст вопроса
        q_match = re.search(r'\*\*Вопрос\.\*\*\s*(.*?)\n(?=- \*\*[A-D]\.)', block, re.DOTALL)
        question_text = q_match.group(1).strip() if q_match else ""

        # 3. Извлекаем опции (A, B, C, D)
        options = []
        opt_pattern = re.finditer(r'- \*\*([A-D])\.\*\*\s*(.*?)(?=\n- \*\*[A-D]\.\*\*|\n\n\*\*Ответ)', block, re.DOTALL)
        for opt in opt_pattern:
            options.append({"id": opt.group(1), "text": opt.group(2).strip()})

        # 4. Извлекаем правильный ответ (например, "B, C")
        ans_match = re.search(r'\*\*Ответ:\*\*\s*([A-D,\s]+)', block)
        answer_ids = []
        if ans_match:
            for a in ans_match.group(1).split(','):
                if a.strip() != "":
                    answer_ids.append(a.strip())

        # 5. Извлекаем Rationale (обоснование)
        rat_match = re.search(r'\*\*Ответ:\*\*.*?\n\n(.*?)(?=\n\n\*\*Источник:|\n\n\*\*Типичная ошибка:)', block,
                              re.DOTALL)
        rationale = rat_match.group(1).strip() if rat_match else ""

        # 6. Извлекаем страницы (источники)
        src_match = re.search(r'\*\*Источник:\*\*.*?PDF с\.\s*([0-9,\s]+)', block)
        pages = [int(p.strip()) for p in src_match.group(1).split(',') if p.strip().isdigit()] if src_match else []

        # Формируем JSONL записи
        questions.append({
            "id": q_id,
            "question": question_text,
            "options": options
        })

        gold.append({
            "id": q_id,
            "answer_ids": answer_ids,
            "abstain": len(answer_ids) == 0,
            "rationale": rationale,
            "citations_pages": pages,  # Временно сохраняем страницы, позже замапим на block_id
            "citations": []  # Пусто, пока не свяжем с корпусом
        })

    return questions, gold


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--input_md", required=True)
    args = parser.parse_args()

    qs, golds = parse_qa_markdown(args.input_md)

    with open("dev_questions.jsonl", "w", encoding="utf-8") as f:
        for q in qs: f.write(json.dumps(q, ensure_ascii=False) + "\n")

    with open("dev_gold.jsonl", "w", encoding="utf-8") as f:
        for g in golds: f.write(json.dumps(g, ensure_ascii=False) + "\n")

    print(f"Saved {len(qs)} questions and gold standards.")