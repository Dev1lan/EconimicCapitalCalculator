"""Экспорт нормализованных сделок по контракту INP-14."""

import csv
from datetime import date
import math
from pathlib import Path

import polars as pl
import pyarrow as pa
import pyarrow.parquet as pq

from normalizer.processor import OUTPUT_COLUMNS, OUTPUT_COLUMNS_V2


_REQUIRED = {
    "deal_id", "report_dttm", "discriminator", "currency_id", "side", "portfolio_key",
    "current_principal_amt", "deal_dttm", "maturity_dttm", "rate_type_cd", "interest_rate",
    "day_convention_cd",
}
_DATES = {
    "report_dttm", "deal_dttm", "maturity_dttm",
    "interest_payment_rule_start_dttm", "regular_payment_rule_start_dttm",
    "repricing_rule_start_dttm",
}
_FLOATS = {
    "current_principal_amt", "interest_rate", "base_indicator_rate",
    "cap_limit_interest_rate", "floor_limit_interest_rate", "accrued_interest",
}
_INT32 = {
    "portfolio_key", "interest_payment_rule_day_num", "regular_payment_rule_day_num",
    "repricing_rule_day_num", "indicator_lag_days",
}


def _check_deals(deals: pl.DataFrame) -> None:
    if tuple(deals.columns) not in (OUTPUT_COLUMNS, OUTPUT_COLUMNS_V2):
        raise ValueError("Колонки или их порядок не соответствуют контракту INP-14")
    for name in _REQUIRED:
        if deals.get_column(name).null_count() or (deals.get_column(name) == "").any():
            raise ValueError(f"Обязательное поле {name} содержит пустые значения")


def _write_table_csv(data: pl.DataFrame, path: str | Path, *, delimiter: str) -> Path:
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, delimiter=delimiter, quoting=csv.QUOTE_NONE, lineterminator="\n")
        writer.writerow(data.columns)
        writer.writerows(data.iter_rows())
    return target


def write_csv(deals: pl.DataFrame, path: str | Path) -> Path:
    """Создать CSV v1 или v2 для cfcore::CsvDealReader."""
    _check_deals(deals)
    return _write_table_csv(deals, path, delimiter=";")


def write_quarantine(quarantine: pl.DataFrame, path: str | Path) -> Path:
    """Создать файл отклонённых сделок: deal_id,reason_code."""
    if quarantine.columns != ["deal_id", "reason_code"]:
        raise ValueError("Ожидаются колонки deal_id, reason_code")
    return _write_table_csv(quarantine, path, delimiter=",")


def parquet_schema(columns=OUTPUT_COLUMNS) -> pa.Schema:
    """Физическая Arrow/Parquet-схема выбранного контракта."""
    fields = []
    for name in columns:
        if name == "deal_id":
            dtype = pa.int64()
        elif name in _INT32:
            dtype = pa.int32()
        elif name in _FLOATS:
            dtype = pa.float64()
        elif name in _DATES:
            dtype = pa.date32()
        else:
            dtype = pa.string()
        fields.append(pa.field(name, dtype, nullable=name not in _REQUIRED))
    return pa.schema(fields)


def _arrow_values(values: list[str | None], dtype: pa.DataType) -> list:
    converted = []
    for value in values:
        if value is None:
            converted.append(None)
        elif pa.types.is_date32(dtype):
            converted.append(date.fromisoformat(value))
        elif pa.types.is_integer(dtype):
            converted.append(int(value))
        elif pa.types.is_floating(dtype):
            number = float(value)
            if not math.isfinite(number):
                raise ValueError("Parquet не допускает NaN и бесконечные ставки/суммы")
            converted.append(number)
        else:
            converted.append(value)
    return converted


def write_parquet(deals: pl.DataFrame, path: str | Path) -> Path:
    """Создать типизированную Parquet-проекцию (само C++-ядро её пока не читает)."""
    _check_deals(deals)
    schema = parquet_schema(deals.columns)
    arrays = [
        pa.array(_arrow_values(deals.get_column(field.name).to_list(), field.type), type=field.type)
        for field in schema
    ]
    table = pa.Table.from_arrays(arrays, schema=schema)
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    pq.write_table(table, target, compression="zstd")
    return target
