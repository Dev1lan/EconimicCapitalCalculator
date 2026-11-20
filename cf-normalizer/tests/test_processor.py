"""INP-2: контракт и проверки нормализатора."""

import json
from pathlib import Path

import polars as pl
import pytest

from normalizer.processor import OUTPUT_COLUMNS, normalize_deals
from normalizer.reader import read_csv


ROOT = Path(__file__).resolve().parents[1]
SAMPLE = ROOT / "tests" / "data" / "тестовые_сделки_МАИ.csv"

BASE = {
    "deal_id": "1",
    "report_dttm": "31.08.2022",
    "discriminator": "SIMPLE",
    "currency_id": "RUB",
    "current_principal_amt": "1000.00",
    "deal_dttm": "22.06.2020",
    "maturity_dttm": "15.06.2050",
    "rate_type_cd": "FIXED",
    "interest_rate": "0.075",
    "day_convention_cd": "ACT/ACT",
}


def deals(*changes: dict[str, str | None]) -> pl.DataFrame:
    rows = [{**BASE, **row} for row in changes] or [BASE.copy()]
    return pl.DataFrame(rows, schema={name: pl.String for name in rows[0]})


def result_of(**changes: str | None):
    return normalize_deals(deals(changes))


def test_contract_fields_match_json_schema():
    path = ROOT / "contracts" / "normalized-deals-v1.json"
    schema = json.loads(path.read_text(encoding="utf-8"))
    assert schema["version"] == "1.0.0"
    assert tuple(field["name"] for field in schema["fields"]) == OUTPUT_COLUMNS
    assert len(OUTPUT_COLUMNS) == len(set(OUTPUT_COLUMNS)) == 29


def test_all_55_sample_deals():
    result = normalize_deals(read_csv(SAMPLE))
    assert result.deals.shape == (45, 29)
    assert result.quarantine.shape == (10, 2)
    assert result.deals.columns == list(OUTPUT_COLUMNS)
    assert all(t == pl.String for t in result.deals.schema.values())
    actual = set(result.quarantine.iter_rows())
    assert actual == {("32", "UNSUPPORTED_CURRENCY")} | {
        (str(i), "SUB_BLOCK_UNSUPPORTED") for i in range(47, 56)
    }
    ids = set(result.deals.get_column("deal_id").to_list())
    assert {"13", "20", "15"} <= ids
    assert "32" not in ids
    assert result.deals.filter(pl.col("deal_id") == "15")["side"][0] == "LIABILITY"


def test_csv_not_converted_to_cashflows():
    result = result_of(discriminator="ANN", regular_payment_rule_period_cd="1M")
    assert result.deals["principal_payment"][0] is None
    assert result.deals["regular_payment_rule_period_cd"][0] == "1M"


def test_ann_non_monthly_rule_is_left_for_core_validation():
    result = result_of(discriminator="ANN", regular_payment_rule_period_cd="45D")
    assert result.quarantine.is_empty()
    assert result.deals["regular_payment_rule_period_cd"][0] == "45D"


def test_mappings_dates_and_zero():
    result = result_of(
        report_dttm="2022-08-31", day_convention_cd="ACT/365",
        accrued_interest="0.00", current_principal_amt="-1000.50",
    )
    row = result.deals.row(0, named=True)
    assert row["side"] == "LIABILITY"
    assert row["current_principal_amt"] == "1000.50"
    assert row["report_dttm"] == "2022-08-31"
    assert row["day_convention_cd"] == "ACT_365"
    assert row["accrued_interest"] == "0.00"
    assert result_of().deals["accrued_interest"][0] is None


def test_portfolio_key_first_accepted_occurrence():
    frame = deals(
        {"deal_id": "1", "product": "mortgage", "behavior_model_cd": "A"},
        {"deal_id": "2", "product": "deposits", "behavior_model_cd": "B"},
        {"deal_id": "3", "product": "mortgage", "behavior_model_cd": "A"},
        {"deal_id": "4", "product": "ignored", "currency_id": "USD"},
        {"deal_id": "5", "product": "product", "behavior_model_cd": ""},
    )
    result = normalize_deals(frame)
    assert result.deals["portfolio_key"].to_list() == ["0", "1", "0", "2"]
    assert result.portfolio_keys == {("mortgage", "A"): 0, ("deposits", "B"): 1, ("", ""): 2}


def test_float_external_fixings_optional():
    frame = deals({"rate_type_cd": "FLOAT", "base_indicator_rate": "0.08", "interest_rate": "0.03"})
    assert normalize_deals(frame).deals["base_indicator_rate"][0] is None
    assert normalize_deals(frame, use_external_fixings=False).deals["base_indicator_rate"][0] == "0.08"
    assert normalize_deals(frame).deals["interest_rate"][0] == "0.03"


def test_schedules_sorted_stably_without_losing_duplicates():
    result = result_of(
        interest_payment="2022-10-31 31.08.2022 2022-10-31",
        principal_payment="2022-10-31,20 2022-09-01,10.00 2022-10-31,30",
        repricing_dttm_schedule="2023-01-09 31.08.2022",
        repricing_dttm_rate_schedule_fixed="2022-10-01,0.1 2022-08-31,0.05",
    )
    row = result.deals.row(0, named=True)
    assert row["interest_payment"] == "2022-08-31 2022-10-31 2022-10-31"
    assert row["principal_payment"] == "2022-09-01,10.00 2022-10-31,20 2022-10-31,30"
    assert row["repricing_dttm_schedule"] == "2022-08-31 2023-01-09"
    assert row["repricing_dttm_rate_schedule_fixed"] == "2022-08-31,0.05 2022-10-01,0.1"


def test_start_without_period_is_allowed():
    result = result_of(repricing_rule_start_dttm="20.01.2022")
    assert result.quarantine.is_empty()
    assert result.deals["repricing_rule_start_dttm"][0] == "2022-01-20"
    assert result.deals["repricing_rule_period_cd"][0] is None


def test_repricing_last_day_but_not_interest_last_day():
    assert result_of(repricing_rule_day_num="-1").deals["repricing_rule_day_num"][0] == "-1"
    assert result_of(interest_payment_rule_day_num="-1").quarantine["reason_code"][0] == "INVALID_RULE"


def test_extra_columns_are_ignored_and_optional_columns_may_be_absent():
    result = result_of(unknown_field="ignored")
    assert result.deals.shape == (1, 29)
    assert "unknown_field" not in result.deals.columns


def test_missing_required_header_is_fatal():
    with pytest.raises(ValueError, match="interest_rate"):
        normalize_deals(pl.DataFrame({"deal_id": ["1"]}))


def test_empty_input_preserves_output_schema():
    frame = pl.DataFrame(schema={name: pl.String for name in BASE})
    result = normalize_deals(frame)
    assert result.deals.shape == (0, 29)
    assert result.quarantine.shape == (0, 2)


@pytest.mark.parametrize(
    ("changes", "error"),
    [
        ({"currency_id": "USD", "sub_rate_type_cd": "FLOAT"}, "UNSUPPORTED_CURRENCY"),
        ({"sub_rate_type_cd": "FLOAT"}, "SUB_BLOCK_UNSUPPORTED"),
        ({"sub_interest_rate": "0.1"}, "SUB_BLOCK_UNSUPPORTED"),
        ({"deal_id": ""}, "MISSING_REQUIRED"),
        ({"deal_id": "abc"}, "INVALID_ID"),
        ({"deal_id": str(2**63)}, "INVALID_ID"),
        ({"deal_id": "9" * 5000}, "INVALID_ID"),
        ({"discriminator": "FLOAT"}, "INVALID_ENUM"),
        ({"rate_type_cd": "UNKNOWN"}, "INVALID_ENUM"),
        ({"day_convention_cd": "30/360"}, "INVALID_ENUM"),
        ({"report_dttm": "29.02.2023"}, "INVALID_DATE"),
        ({"report_dttm": "31.08.2201"}, "INVALID_DATE"),
        ({"maturity_dttm": "2022-08-31"}, "INVALID_DATE"),
        ({"current_principal_amt": "0"}, "INVALID_NUMBER"),
        ({"current_principal_amt": "NaN"}, "INVALID_NUMBER"),
        ({"current_principal_amt": "1e309"}, "INVALID_NUMBER"),
        ({"interest_rate": "0,075"}, "INVALID_NUMBER"),
        ({"interest_rate": "-0.01"}, "INVALID_NUMBER"),
        ({"floor_limit_interest_rate": "0.10", "cap_limit_interest_rate": "0.05"}, "INVALID_NUMBER"),
        ({"interest_payment_rule_period_cd": "0M"}, "INVALID_RULE"),
        ({"interest_payment_rule_period_cd": "99999999999D"}, "INVALID_RULE"),
        ({"interest_payment_rule_period_cd": "9" * 5000 + "M"}, "INVALID_RULE"),
        ({"repricing_rule_day_num": "32"}, "INVALID_RULE"),
        ({"interest_payment": "2022-13-01"}, "INVALID_SCHEDULE"),
        ({"principal_payment": "2022-09-01,-1"}, "INVALID_SCHEDULE"),
        ({"principal_payment": "2022-09-01,foo"}, "INVALID_SCHEDULE"),
        ({"principal_payment": "2022-09-01"}, "INVALID_SCHEDULE"),
    ],
)
def test_bad_row_goes_to_quarantine(changes, error):
    result = result_of(**changes)
    assert result.deals.is_empty()
    assert result.quarantine["reason_code"].to_list() == [error]


def test_bad_row_does_not_stop_next_rows():
    frame = deals(
        {"deal_id": "1", "interest_rate": "bad"},
        {"deal_id": "2", "interest_rate": "0.05"},
    )
    result = normalize_deals(frame)
    assert result.deals["deal_id"].to_list() == ["2"]
    assert result.quarantine["reason_code"].to_list() == ["INVALID_NUMBER"]


def test_repeated_identifier_is_not_passed_twice():
    result = normalize_deals(deals({"deal_id": "001"}, {"deal_id": "1"}))
    assert result.deals.is_empty()
    assert result.quarantine["reason_code"].to_list() == ["INVALID_ID", "INVALID_ID"]


def test_negative_float_spread_is_allowed():
    result = result_of(rate_type_cd="FLOAT", interest_rate="-0.01")
    assert result.quarantine.is_empty()
    assert result.deals["interest_rate"][0] == "-0.01"
