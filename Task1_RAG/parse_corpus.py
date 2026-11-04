import fitz  # pip install pymupdf
import json
import argparse


def parse_pdf_to_corpus(pdf_path: str, output_path: str):
    doc = fitz.open(pdf_path)
    block_counter = 1

    with open(output_path, "w", encoding="utf-8") as f:
        for page_num in range(len(doc)):
            page = doc[page_num]
            # get_text("blocks") возвращает список кортежей: (x0, y0, x1, y1, "text", block_no, block_type)
            # block_type 0 = текст, 1 = изображение
            blocks = page.get_text("blocks")

            # Сортируем блоки по координатам Y (сверху вниз), затем X
            blocks.sort(key=lambda b: (b[1], b[0]))

            for b in blocks:
                if b[6] == 0:  # Только текстовые блоки
                    text = b[4].strip()
                    if len(text) > 10:  # Игнорируем короткие колонтитулы
                        block_id = f"b_{block_counter:04d}"
                        record = {
                            "block_id": block_id,
                            "page": page_num + 1,  # Страницы с 1
                            "section": "unknown",  # Если есть парсер заголовков, можно подставить
                            "text": text
                        }
                        f.write(json.dumps(record, ensure_ascii=False) + "\n")
                        block_counter += 1
    print(f"Extracted {block_counter - 1} blocks.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--pdf", required=True)
    parser.add_argument("--output", default="corpus.jsonl")
    args = parser.parse_args()
    parse_pdf_to_corpus(args.pdf, args.output)