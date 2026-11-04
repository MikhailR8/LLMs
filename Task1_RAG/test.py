import os
import re
import json

from marker.converters.pdf import PdfConverter
from marker.models import create_model_dict
from marker.output import text_from_rendered


# Твоя функция для фикса кодировки
def fix_cp1251_mojibake(text: str) -> str:
    out = []
    for ch in text:
        try:
            b = ch.encode("cp1252")
        except UnicodeEncodeError:
            out.append(ch); continue
        if len(b) != 1 or b[0] < 0x80: out.append(ch); continue
        try:
            cand = b.decode("cp1251")
        except UnicodeDecodeError:
            out.append(ch); continue
        out.append(cand)
    return "".join(out)


print("Инициализация моделей (первый запуск скачает веса Surya с HuggingFace)...")
converter = PdfConverter(artifact_dict=create_model_dict())

# Замени "tfkp_textbook.pdf" на путь к твоему учебнику
rendered = converter("TFKP.pdf")
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
                corpus.append(
                    {"block_id": f"b_{block_counter:04d}", "page": page_num, "section": current_section, "text": text})
                block_counter += 1
        else:
            if len(text) < 5: continue
            corpus.append(
                {"block_id": f"b_{block_counter:04d}", "page": page_num, "section": current_section, "text": text})
            block_counter += 1

# Сохраняем корпус
with open("corpus.jsonl", "w", encoding="utf-8") as f:
    for c in corpus:
        f.write(json.dumps(c, ensure_ascii=False) + "\n")

print(f"Готово! Извлечено {len(corpus)} блоков. Формулы должны быть в формате LaTeX ($$ ... $$).")