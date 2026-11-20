# Эталонные тесты CF Normalizer

Эталоны получены на 55 тестовых сделках МАИ. Результаты нормализации сверены с `tools/normalize_stub.py`, результаты расчёта — с `cfcore` из [`Dev1lan/EconimicCapitalCalculator`](https://github.com/Dev1lan/EconimicCapitalCalculator/tree/develop/core), коммит `6fa3b5c`.

## Эталонные файлы

| Файл | Содержимое |
|---|---|
| `tests/data/тестовые_сделки_МАИ.csv` | 55 исходных сделок |
| `tests/golden/normalized_deals.csv` | 45 нормализованных сделок, 29 полей |
| `tests/golden/quarantine.csv` | 10 отклонённых сделок |
| `tests/golden/core_deal_summary.csv` | Результаты ядра для 44 сделок |
| `tests/golden/core_errors.csv` | Ошибка расчёта сделки №20 |

## Ожидаемый результат

| Проверка | Результат |
|---|---|
| Нормализация | 45 принятых сделок |
| Карантин | №32: `UNSUPPORTED_CURRENCY`; №47–55: `SUB_BLOCK_UNSUPPORTED` |
| C++-ядро | 44 рассчитанные сделки, 3965 денежных потоков |
| Ошибки ядра | №20: `SCHEDULE_SUM_MISMATCH` |

Тесты сравнивают все 29 полей CSV/Parquet, типы и пустые значения, карантин, расчётную сводку и число денежных потоков. Для чисел в сводке ядра допуск: абсолютный 0,02, относительный 1e-9.

## Запуск

Из каталога `cf-normalizer`:

```bash
python -m pip install -e ".[dev]"
python -m pytest tests/ -q
```

Для тестирования настоящего C++-ядра дополнительно требуются собранный `cfcore` и файл `data/input/key_rate.csv` из указанного коммита ядра.

Linux/macOS:

```bash
export CFCORE_BIN=/path/to/cfcore
export CFCORE_FIXINGS=/path/to/data/input/key_rate.csv
python -m pytest tests/ -q
```

Windows PowerShell:

```powershell
$env:CFCORE_BIN = 'C:\path\to\cfcore.exe'
$env:CFCORE_FIXINGS = 'C:\path\to\data\input\key_rate.csv'
python -m pytest tests/ -q
```

Если обе переменные не заданы, тест C++-ядра пропускается. Файл ставок проверяется по SHA-256; эталонные данные соответствуют только закреплённой версии ядра.

Эталоны обновляются при согласованном изменении контракта или версии ядра. Ошибка №20 не исправляется нормализатором.
