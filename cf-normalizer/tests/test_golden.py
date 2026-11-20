"""INP-4: независимые эталоны из временного нормализатора C++-команды."""

import csv
from decimal import Decimal
import json
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq

from normalizer.processor import OUTPUT_COLUMNS, normalize_deals
from normalizer.reader import read_csv
from normalizer.writer import write_csv, write_parquet, write_quarantine


ROOT = Path(__file__).resolve().parents[1]
SAMPLE = ROOT / "tests/data/тестовые_сделки_МАИ.csv"
GOLDEN = ROOT / "tests/golden"


def _read_rows(path: Path, *, delimiter: str) -> tuple[list[str], list[dict[str, str]]]:
    with path.open(encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter=delimiter)
        return list(reader.fieldnames or []), list(reader)


def _assert_normalized_rows(actual: list[dict], expected: list[dict]) -> None:
    # Различие текстовых представлений числа (например, 100.0 / 100.00)
    # допустимо: для ядра и Parquet эти значения эквивалентны.
    contract = json.loads((ROOT / "contracts/normalized-deals-v1.json").read_text(encoding="utf-8"))
    numeric = {
        field["name"] for field in contract["fields"]
        if field["parquet_type"] in {"int32", "int64", "float64"}
    }
    assert len(actual) == len(expected) == 45
    for row, reference in zip(actual, expected, strict=True):
        assert set(row) == set(reference) == set(OUTPUT_COLUMNS)
        assert row["deal_id"] == reference["deal_id"]
        for name in OUTPUT_COLUMNS:
            value = row[name] if row[name] is not None else ""
            target = reference[name] if reference[name] is not None else ""
            if name in numeric and value != "" and target != "":
                assert Decimal(value) == Decimal(target), (row["deal_id"], name)
            else:
                assert value == target, (row["deal_id"], name)


def test_all_55_deals_match_independent_reference():
    """Сравнить все 29 колонок, все 45 сделок и весь карантин."""
    source = read_csv(SAMPLE)
    assert source.shape == (55, 48)
    result = normalize_deals(source, use_external_fixings=True)
    columns, expected = _read_rows(GOLDEN / "normalized_deals.csv", delimiter=";")
    assert columns == list(OUTPUT_COLUMNS)
    _assert_normalized_rows(result.deals.to_dicts(), expected)

    columns, expected_quarantine = _read_rows(GOLDEN / "quarantine.csv", delimiter=",")
    assert columns == ["deal_id", "reason_code"]
    assert result.quarantine.to_dicts() == expected_quarantine
    assert len(result.deals) + len(result.quarantine) == len(source)


def test_exported_csv_and_parquet_match_reference(tmp_path):
    """Проверить границу writer: CSV, карантин и каждый типизированный столбец Parquet."""
    normalized = normalize_deals(read_csv(SAMPLE), use_external_fixings=True)
    path = write_csv(normalized.deals, tmp_path / "deals.csv")
    columns, rows = _read_rows(path, delimiter=";")
    _, expected = _read_rows(GOLDEN / "normalized_deals.csv", delimiter=";")
    assert columns == list(OUTPUT_COLUMNS)
    _assert_normalized_rows(rows, expected)

    rejected = write_quarantine(normalized.quarantine, tmp_path / "quarantine.csv")
    expected_quarantine = (GOLDEN / "quarantine.csv").read_bytes().replace(b"\r\n", b"\n")
    assert rejected.read_bytes() == expected_quarantine

    parquet = write_parquet(normalized.deals, tmp_path / "deals.parquet")
    table = pq.read_table(parquet)
    assert table.num_rows == len(expected)
    assert table.column_names == list(OUTPUT_COLUMNS)
    for field in table.schema:
        values = table[field.name].to_pylist()
        for value, reference in zip(values, expected, strict=True):
            raw = reference[field.name]
            if raw == "":
                assert value is None, (reference["deal_id"], field.name)
            elif pa.types.is_date32(field.type):
                assert value.isoformat() == raw, (reference["deal_id"], field.name)
            elif pa.types.is_integer(field.type):
                assert value == int(raw), (reference["deal_id"], field.name)
            elif pa.types.is_floating(field.type):
                assert abs(Decimal(str(value)) - Decimal(raw)) <= Decimal("0.00000001"), (
                    reference["deal_id"], field.name
                )
            else:
                assert value == raw, (reference["deal_id"], field.name)


def test_fixings_switch_only_changes_floating_base_indicator():
    """Без истории ключевой ставки нормализатор сохраняет ставку из исходника."""
    source = read_csv(SAMPLE)
    external = normalize_deals(source, use_external_fixings=True).deals.to_dicts()
    embedded = normalize_deals(source, use_external_fixings=False).deals.to_dicts()
    raw_by_id = {row["deal_id"]: row for row in source.iter_rows(named=True)}

    floating = 0
    for with_fixings, without_fixings in zip(external, embedded, strict=True):
        deal_id = with_fixings["deal_id"]
        for name in OUTPUT_COLUMNS:
            if name != "base_indicator_rate" or with_fixings["rate_type_cd"] != "FLOAT":
                assert with_fixings[name] == without_fixings[name], (deal_id, name)
        if with_fixings["rate_type_cd"] == "FLOAT":
            floating += 1
            assert with_fixings["base_indicator_rate"] is None
            assert without_fixings["base_indicator_rate"] == raw_by_id[deal_id]["base_indicator_rate"]
    assert floating > 0
