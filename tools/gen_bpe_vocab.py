#!/usr/bin/env python3
"""Генерирует словарь ssentencepiece ("токен score") из бинарного bpe.model.

Зачем: у zipformer-ru токены — BPE-куски. Чтобы в hotwords можно было писать
обычные русские слова, sherpa-onnx нужен текстовый словарь формата
"<токен> <score>" (опции --modeling-unit=bpe --bpe-vocab=...). Бинарный
bpe.model для этого НЕ подходит: ssentencepiece::LoadVocab ждёт две колонки
и на бинарнике падает с сообщением
"Each line in vocab should contain two items (seperate by space)".

Установка и запуск:
    pip install sentencepiece
    python3 gen_bpe_vocab.py ~/.voice_models/small-zipformer-ru/bpe.model \
                             ~/.voice_models/small-zipformer-ru/bpe.vocab

Полученный bpe.vocab передаётся в ZipformerRecognizer::Options::bpeVocab.
"""

import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    model_path, out_path = sys.argv[1], sys.argv[2]

    try:
        import sentencepiece as spm
    except ImportError:
        print("нужен пакет sentencepiece:  pip install sentencepiece", file=sys.stderr)
        return 1

    sp = spm.SentencePieceProcessor()
    if not sp.Load(model_path):
        print("не смог загрузить модель: %s" % model_path, file=sys.stderr)
        return 1

    written = skipped = 0
    with open(out_path, "w", encoding="utf-8") as f:
        for i in range(sp.GetPieceSize()):
            piece = sp.IdToPiece(i)
            # ssentencepiece читает пары "токен score" через operator>> ,
            # поэтому токены с пробелами недопустимы
            if not piece or any(ch.isspace() for ch in piece):
                skipped += 1
                continue
            f.write("%s %.6f\n" % (piece, sp.GetScore(i)))
            written += 1

    print("кусков в модели: %d | записано: %d | пропущено: %d -> %s"
          % (sp.GetPieceSize(), written, skipped, out_path))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
