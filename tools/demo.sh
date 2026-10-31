#!/usr/bin/env bash
# Полный прогон для демонстрации: сборка, тесты, 55 сделок МАИ, синтетический портфель.
# Запуск из корня репозитория: bash tools/demo.sh [число_сделок]
set -euo pipefail

DEALS="${1:-10000}"
PYTHON="${PYTHON:-python3}"

echo "== Сборка и тесты"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build build -j8 > /dev/null
ctest --test-dir build 2>&1 | grep -E "tests passed|tests failed"

echo "== Тестовые сделки МАИ через временный нормализатор"
"$PYTHON" tools/normalize_stub.py data/input/test_deals_mai.csv out/mai/deals.csv out/mai/quarantine.csv
./build/core/cfcore --input out/mai/deals.csv --output-dir out/mai/run --fixings data/input/key_rate.csv
"$PYTHON" tools/verify_output.py out/mai/run

echo "== Синтетический портфель: $DEALS сделок, seed 42, 0.1% намеренно испорчено"
"$PYTHON" tools/gen_synthetic.py out/synthetic/deals.csv --deals "$DEALS" --seed 42 --bad-share 0.001
./build/core/cfcore --input out/synthetic/deals.csv --output-dir out/synthetic/run
"$PYTHON" tools/verify_output.py out/synthetic/run

echo "== Ошибки в синтетике по типам"
tail -n +2 out/synthetic/run/errors.csv | cut -d, -f2 | sort | uniq -c
