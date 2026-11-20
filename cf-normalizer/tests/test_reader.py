from pathlib import Path

import polars as pl
import pytest

from normalizer.reader import read_csv


DATA = Path(__file__).parent / "data" / "тестовые_сделки_МАИ.csv"


def write_csv(tmp_path: Path, text: str) -> Path:
    path = tmp_path / "deals.csv"
    path.write_text(text, encoding="utf-8")
    return path


def test_load_sample_csv():
    deals = read_csv(DATA)

    assert isinstance(deals, pl.DataFrame)
    assert deals.shape == (55, 48)
    assert deals.columns[:4] == [
        "report_dttm", "discriminator", "deal_id", "currency_id"
    ]
    assert deals["deal_id"][0] == "1"
    assert deals["discriminator"].unique().sort().to_list() == [
        "ANN", "RGM", "SIMPLE"
    ]


def test_all_columns_are_strings():
    deals = read_csv(DATA)

    assert all(dtype == pl.String for dtype in deals.schema.values())
    assert deals["interest_rate"][0] == "0.075"
    assert deals["deal_dttm"][0] == "22.06.2020"


def test_empty_fields_are_null():
    deals = read_csv(DATA)

    assert deals["start_principal_amt"][0] is None
    assert deals["principal_payment"][0] is None


def test_preserves_leading_zeros(tmp_path):
    path = write_csv(tmp_path, "deal_id;interest_rate\n001;0.075\n")

    deals = read_csv(path)

    assert deals["deal_id"][0] == "001"
    assert deals["interest_rate"][0] == "0.075"


def test_quoted_separator(tmp_path):
    path = write_csv(tmp_path, 'deal_id;comment\n1;"a;b"\n')

    deals = read_csv(path)

    assert deals.shape == (1, 2)
    assert deals["comment"][0] == "a;b"


def test_does_not_validate_financial_values(tmp_path):
    path = write_csv(
        tmp_path,
        "deal_id;discriminator;maturity_dttm;interest_rate\n"
        "1;UNKNOWN;99.99.9999;not_a_number\n",
    )

    deals = read_csv(path)

    assert deals["discriminator"][0] == "UNKNOWN"
    assert deals["maturity_dttm"][0] == "99.99.9999"
    assert deals["interest_rate"][0] == "not_a_number"


def test_missing_domain_column_is_allowed(tmp_path):
    path = write_csv(tmp_path, "deal_id\n1\n")

    deals = read_csv(path)

    assert deals.shape == (1, 1)
    assert deals.columns == ["deal_id"]


def test_extra_column_is_preserved(tmp_path):
    path = write_csv(tmp_path, "deal_id;extra\n1;value\n")

    deals = read_csv(path)

    assert deals.columns == ["deal_id", "extra"]
    assert deals["extra"][0] == "value"


def test_missing_file_raises(tmp_path):
    with pytest.raises(FileNotFoundError):
        read_csv(tmp_path / "missing.csv")


def test_empty_file_raises(tmp_path):
    path = write_csv(tmp_path, "")

    with pytest.raises(pl.exceptions.NoDataError):
        read_csv(path)


def test_extra_field_raises(tmp_path):
    path = write_csv(tmp_path, "deal_id;rate\n1;0.075;EXTRA\n")

    with pytest.raises(pl.exceptions.PolarsError):
        read_csv(path)
