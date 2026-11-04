# build_index.py
import argparse
import json
from pathlib import Path
from rag_system.indexer import CorpusIndexer


def main():
    parser = argparse.ArgumentParser(description="Build RAG index from corpus")
    parser.add_argument("--corpus", required=True, help="Path to corpus.jsonl")
    parser.add_argument("--output", required=True, help="Output directory for index")
    parser.add_argument("--config", default="runtime.json", help="Path to runtime config")
    args = parser.parse_args()

    # Загружаем конфиг
    with open(args.config, 'r', encoding='utf-8') as f:
        config = json.load(f)

    embedding_model = config.get("retrieval", {}).get("embedding_model", "BAAI/bge-m3")

    # Создаем индексер
    indexer = CorpusIndexer(embedding_model_name=embedding_model)

    # Загружаем корпус
    indexer.load_corpus(args.corpus)

    # Строим индексы
    indexer.build_bm25_index()
    indexer.build_dense_index(batch_size=32)

    # Сохраняем
    indexer.save_index(args.output)
    print("Index building completed successfully!")


if __name__ == "__main__":
    main()
