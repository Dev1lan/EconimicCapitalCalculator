#!/usr/bin/env bash
# Полный прогон для демонстрации: сборка, тесты, 55 сделок МАИ, синтетический портфель.
# Подготовка: python -m pip install -e ./cf-normalizer
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

DEALS="${1:-10000}"
PYTHON="${PYTHON:-python3}"

echo "== Сборка и тесты"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build build --config Release -j8 > /dev/null
ctest --test-dir build -C Release --output-on-failure
for CFCORE in build/core/Release/cfcore.exe build/core/cfcore.exe build/core/Release/cfcore build/core/cfcore; do
    if [[ -x "$CFCORE" ]]; then break; fi
done
if [[ ! -x "$CFCORE" ]]; then
    echo "Не найден собранный cfcore" >&2
    exit 1
fi

echo "== Тестовые сделки МАИ"
"$PYTHON" -m normalizer data/input/test_deals_mai.csv --output-dir out/mai --format csv \
    --cfcore "$CFCORE" --fixings data/input/key_rate.csv
"$PYTHON" tools/verify_output.py out/mai/core

echo "== Синтетический портфель: $DEALS сделок, seed 42, 0.1% намеренно испорчено"
"$PYTHON" tools/gen_synthetic.py out/synthetic/deals.csv --deals "$DEALS" --seed 42 --bad-share 0.001
"$CFCORE" --input out/synthetic/deals.csv --output-dir out/synthetic/run
"$PYTHON" tools/verify_output.py out/synthetic/run

echo "== Ошибки в синтетике по типам"
tail -n +2 out/synthetic/run/errors.csv | cut -d, -f2 | sort | uniq -c
