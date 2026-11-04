import argparse
import json
import re

def parse_qa(md_path):
    with open(md_path, 'r', encoding='utf-8') as f:
        content = f.read()

    question_blocks = re.split(r'(?=##\s*\d+\.)', content)
    questions, golds = [], []

    for block in question_blocks:
        if not block.strip().startswith('##'): continue

        id_match = re.search(r'##\s*(\d+)\.', block)
        if not id_match: continue
        q_id = f"q_{int(id_match.group(1)):03d}"

        q_match = re.search(r'\*\*Вопрос\.\*\*\s*(.*?)\n(?=- \*\*[A-D]\.)', block, re.DOTALL)
        question_text = q_match.group(1).strip() if q_match else ""

        options = []
        for opt in re.finditer(r'- \*\*([A-D])\.\*\*\s*(.*?)(?=\n- \*\*[A-D]\.\*\*|\n\n\*\*Ответ)', block, re.DOTALL):
            options.append({"id": opt.group(1), "text": opt.group(2).strip()})

        ans_match = re.search(r'\*\*Ответ:\*\*\s*([A-D,\s]+)', block)
        answer_ids = [a.strip() for a in ans_match.group(1).split(',') if a.strip() != ""] if ans_match else []

        rat_match = re.search(r'\*\*Ответ:\*\*.*?\n\n(.*?)(?=\n\n\*\*Источник:|\n\n\*\*Типичная ошибка:)', block, re.DOTALL)
        rationale = rat_match.group(1).strip() if rat_match else ""

        src_match = re.search(r'\*\*Источник:\*\*.*?PDF с\.\s*([0-9,\s]+)', block)
        pages = [int(p.strip()) for p in src_match.group(1).split(',') if p.strip().isdigit()] if src_match else []

        questions.append({"id": q_id, "question": question_text, "options": options})
        golds.append({
            "id": q_id, "answer_ids": answer_ids, "abstain": len(answer_ids) == 0,
            "rationale": rationale, "citations_pages": pages, "citations": []
        })
    return questions, golds

def fix_cp1251_mojibake(text: str) -> str:
    out = []
    for ch in text:
        try: b = ch.encode("cp1252")
        except UnicodeEncodeError: out.append(ch); continue
        if len(b) != 1 or b[0] < 0x80: out.append(ch); continue
        try: cand = b.decode("cp1251")
        except UnicodeDecodeError: out.append(ch); continue
        out.append(cand)
    return "".join(out)

def extract_corpus_marker(pdf_path):
    print("Обработка корпуса...")
    from marker.converters.pdf import PdfConverter
    from marker.models import create_model_dict
    from marker.config.parser import ConfigParser
    from marker.output import text_from_rendered

    config = {
        "page_range": "0-40",
        "disable_image_extraction": True,
        "paginate_output": True
    }
    config_parser = ConfigParser(config)

    converter = PdfConverter(
        config=config_parser.generate_config_dict(),
        artifact_dict=create_model_dict()
    )
    print("Start converting")
    rendered = converter(pdf_path)
    print("End converting")
    full_text, _, images = text_from_rendered(rendered)
    # Разбиваем на страницы и блоки
    pages = re.split(r'\n\s*\{PAGE_BREAK\}\s*\n|\n\s*-{3,}\s*\n', full_text)

    corpus = []
    block_counter = 1

    for page_idx, page_text in enumerate(pages):
        page_num = page_idx + 1
        page_text = page_text.strip()
        if not page_text: continue

        current_section = "unknown"
        raw_blocks = re.split(r'\n\s*\n', page_text)

        for raw_block in raw_blocks:
            raw_block = raw_block.strip()
            if not raw_block: continue

            text = fix_cp1251_mojibake(raw_block)

            heading_match = re.match(r'^(#+)\s+(.*)', text)
            if heading_match:
                current_section = heading_match.group(2).strip()
                text = text[heading_match.end():].strip()
                if text:
                    corpus.append({"block_id": f"b_{block_counter:04d}", "page": page_num, "section": current_section, "text": text})
                    block_counter += 1
            else:
                if len(text) < 5: continue
                corpus.append({"block_id": f"b_{block_counter:04d}", "page": page_num, "section": current_section, "text": text})
                block_counter += 1

    print(f"Извлечено {len(corpus)} блоков.")
    return corpus

def map_gold_citations(golds, corpus):
    for gold in golds:
        pages = gold["citations_pages"]
        if not pages: continue
        relevant_blocks = [b["block_id"] for b in corpus if b["page"] in pages]
        gold["citations"] = relevant_blocks[:4]
    return golds

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--pdf", default='TFKP.pdf', help="Путь к PDF учебника")
    parser.add_argument("--qa_md", default='tfkp_15_qa.md', help="Путь к MD с вопросами")
    args = parser.parse_args()

    # 1. Парсим QA
    questions, golds = parse_qa(args.qa_md)

    # 2. Парсим Корпус
    corpus = extract_corpus_marker(args.pdf)

    # 3. Мапим цитаты
    golds = map_gold_citations(golds, corpus)

    with open("corpus.jsonl", "w", encoding="utf-8") as f:
        for c in corpus: f.write(json.dumps(c, ensure_ascii=False) + "\n")
    with open("dev_questions.jsonl", "w", encoding="utf-8") as f:
        for q in questions: f.write(json.dumps(q, ensure_ascii=False) + "\n")
    with open("dev_gold.jsonl", "w", encoding="utf-8") as f:
        for g in golds: f.write(json.dumps(g, ensure_ascii=False) + "\n")

    print("Готово! Файлы сохранены.")