import csv
import json
from pathlib import Path

import polars as pl
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from normalizer.processor import normalize_deals
from normalizer.reader import read_csv
from normalizer.writer import parquet_schema, write_csv, write_parquet, write_quarantine


ROOT = Path(__file__).resolve().parents[1]
SAMPLE = ROOT / "tests/data/тестовые_сделки_МАИ.csv"


@pytest.fixture
def normalized():
    return normalize_deals(read_csv(SAMPLE))


def test_normalized_csv_is_readable_by_core(normalized, tmp_path):
    path = write_csv(normalized.deals, tmp_path / "nested/normalized_deals.csv")
    raw = path.read_bytes()
    assert not raw.startswith(b"\xef\xbb\xbf")
    assert b"\r" not in raw
    assert b'"' not in raw
    with path.open(encoding="utf-8", newline="") as stream:
        rows = list(csv.DictReader(stream, delimiter=";"))
    assert len(rows) == 45
    assert list(rows[0]) == normalized.deals.columns
    assert rows[0]["report_dttm"] == "2022-08-31"
    assert rows[0]["interest_rate"] == "0.075"
    assert rows[0]["accrued_interest"] == ""


def test_quarantine_csv_keeps_reason_and_order(normalized, tmp_path):
    path = write_quarantine(normalized.quarantine, tmp_path / "quarantine.csv")
    with path.open(encoding="utf-8", newline="") as stream:
        rows = list(csv.DictReader(stream, delimiter=","))
    assert len(rows) == 10
    assert rows[0] == {"deal_id": "32", "reason_code": "UNSUPPORTED_CURRENCY"}
    assert [row["deal_id"] for row in rows[1:]] == [str(i) for i in range(47, 56)]


def test_parquet_types_match_pinned_json_contract():
    contract = json.loads((ROOT / "contracts/normalized-deals-v1.json").read_text(encoding="utf-8"))
    arrow = parquet_schema()
    expected = {"int64": pa.int64(), "int32": pa.int32(), "float64": pa.float64(),
                "date32": pa.date32(), "utf8": pa.string()}
    assert len(arrow) == len(contract["fields"]) == 29
    for field, spec in zip(arrow, contract["fields"], strict=True):
        assert field.name == spec["name"]
        assert field.type == expected[spec["parquet_type"]]
        assert field.nullable == spec["nullable"]


def test_parquet_data_roundtrips_with_types_and_nulls(normalized, tmp_path):
    path = write_parquet(normalized.deals, tmp_path / "normalized_deals.parquet")
    table = pq.read_table(path)
    assert table.num_rows == 45
    assert table.schema.equals(parquet_schema())
    assert table["deal_id"].to_pylist()[0] == 1
    assert table["report_dttm"].to_pylist()[0].isoformat() == "2022-08-31"
    assert table["current_principal_amt"].to_pylist()[0] == pytest.approx(3646037.89)
    assert table["accrued_interest"].to_pylist()[0] is None
    assert table["side"].to_pylist()[0] == "ASSET"
    assert table["portfolio_key"].to_pylist()[0] == 0
    assert table["interest_payment"].to_pylist()[0] is None


def test_zero_is_not_null_in_parquet(tmp_path):
    source = pl.DataFrame({
        "deal_id": ["1"], "report_dttm": ["31.08.2022"],
        "discriminator": ["SIMPLE"], "currency_id": ["RUB"],
        "current_principal_amt": ["1000"], "deal_dttm": ["01.01.2022"],
        "maturity_dttm": ["01.01.2023"], "rate_type_cd": ["FIXED"],
        "interest_rate": ["0.05"], "day_convention_cd": ["ACT/365"],
        "accrued_interest": ["0.00"],
    })
    normalized = normalize_deals(source)
    table = pq.read_table(write_parquet(normalized.deals, tmp_path / "deals.parquet"))
    assert table["accrued_interest"].to_pylist() == [0.0]
    assert table["base_indicator_rate"].to_pylist() == [None]


def test_empty_output_still_has_required_headers_and_schema(tmp_path):
    source = pl.DataFrame(schema={name: pl.String for name in (
        "deal_id", "report_dttm", "discriminator", "currency_id", "current_principal_amt",
        "deal_dttm", "maturity_dttm", "rate_type_cd", "interest_rate", "day_convention_cd",
    )})
    result = normalize_deals(source)
    csv_path = write_csv(result.deals, tmp_path / "empty.csv")
    parquet_path = write_parquet(result.deals, tmp_path / "empty.parquet")
    assert len(csv_path.read_text(encoding="utf-8").splitlines()) == 1
    assert pq.read_table(parquet_path).num_rows == 0
    assert pq.read_table(parquet_path).num_columns == 29


def test_writer_rejects_wrong_schema_and_missing_required(tmp_path, normalized):
    with pytest.raises(ValueError, match="контракту"):
        write_csv(normalized.deals.drop("deal_id"), tmp_path / "bad.csv")
    changed = normalized.deals.with_columns(pl.lit(None, dtype=pl.String).alias("deal_id"))
    with pytest.raises(ValueError, match="deal_id"):
        write_parquet(changed, tmp_path / "bad.parquet")


def test_writer_rejects_wrong_quarantine_schema(tmp_path, normalized):
    with pytest.raises(ValueError, match="deal_id"):
        write_quarantine(normalized.quarantine.drop("deal_id"), tmp_path / "bad.csv")
