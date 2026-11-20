# CF Normalizer

Python-модуль для преобразования банковских сделок из CSV в формат C++-ядра `cfcore`.

## Установка

Требуется Python 3.11+.

```bash
python -m pip install -e ".[dev]"
```

## Запуск

Из каталога `cf-normalizer`:

```bash
python -m normalizer tests/data/тестовые_сделки_МАИ.csv --output-dir out
```

Результаты в `out/`:

| Файл | Содержимое |
|---|---|
| `normalized_deals.csv` | 29 полей, разделитель `;`; вход для `cfcore` |
| `normalized_deals.parquet` | Те же поля с типами и `null` |
| `quarantine.csv` | Отклонённые сделки: `deal_id,reason_code` |

Доступные параметры:

| Параметр | Назначение |
|---|---|
| `--format` | `csv`, `parquet` или `both` (по умолчанию `both`) |
| `--cfcore PATH` | Запуск C++-ядра после нормализации |
| `--fixings PATH` | CSV истории ставки `date,value` |
| `--threads N` | Число потоков C++-ядра |

Запуск с ядром:

```bash
python -m normalizer tests/data/тестовые_сделки_МАИ.csv --output-dir out --cfcore /path/to/cfcore --fixings /path/to/key_rate.csv
```

Ядро читает только `normalized_deals.csv`. При `--cfcore` нельзя выбирать `--format parquet`. Результаты ядра записываются в `out/core/`.

Для сделок `FLOAT` при `--fixings` поле `base_indicator_rate` оставляется пустым; без `--fixings` сохраняется исходное значение.

## Python API

```python
from normalizer.reader import read_csv
from normalizer.processor import normalize_deals

result = normalize_deals(read_csv("tests/data/тестовые_сделки_МАИ.csv"))
# result.deals, result.quarantine, result.portfolio_keys
```

`normalize_deals(..., use_external_fixings=True)` по умолчанию использует режим внешней истории ставки. Запись файлов выполняет `normalizer.writer`.

## Тесты

```bash
python -m pytest tests/ -q
```

Для интеграционных тестов задайте `CFCORE_BIN` с путём к собранному ядру. По умолчанию тест МАИ использует файл ставок из репозитория.

Для расчёта ручного JSON добавьте `--manual` к команде запуска с `--cfcore`. Портфельный режим принимает флаги `--events`, `--trace-deal ID`, `--no-flows` и `--excel-etalon`. [Команды и примеры для Windows и POSIX](../docs/cf-integration.md)

## Спецификации

- [Контракт CSV/Parquet и правила обработки](docs/INP-14-data-contract-v1.md)
- [Эталонные результаты и интеграционные тесты](docs/INP-4-golden-tests.md)
- [JSON-схема 29 полей](contracts/normalized-deals-v1.json)
