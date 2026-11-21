"""Independent Decimal reference payments for the release policies."""

import csv
from decimal import Decimal
import json
from pathlib import Path
import subprocess

import pytest

from normalizer.__main__ import main
from normalizer.processor import OUTPUT_COLUMNS, OUTPUT_COLUMNS_V2
from test_contractual_v2 import binary  # noqa: F401


ROOT = Path(__file__).resolve().parents[2]
D = Decimal
BALANCE = D("100000")
EPSILON = D("0.0000001")
DATES = ("2022-09-30", "2022-10-31", "2022-11-30")
DAYS = (30, 31, 30)
INPUT = {
    "deal_id": "46", "report_dttm": "2022-08-31", "deal_dttm": "2022-08-31",
    "maturity_dttm": DATES[-1], "discriminator": "SIMPLE", "currency_id": "RUB",
    "current_principal_amt": str(BALANCE), "rate_type_cd": "FIXED",
    "interest_rate": ".12", "day_convention_cd": "ACT/365",
    "interest_payment": " ".join(DATES),
}
MONTHLY_BODY = {"regular_payment_rule_start_dttm": "2022-08-31",
                "regular_payment_rule_period_cd": "1M", "regular_payment_rule_day_num": "31"}


def interest(balance, rate, days):
    return balance * D(rate) * days / 365


def monthly_payment(balance, rate, count):
    monthly = D(rate) / 12
    return balance * monthly / (1 - (1 + monthly) ** -count)


def expected_annuity(balance, initial, first_interest, subsequent_rate):
    planned = monthly_payment(balance, initial, 3)
    first_principal = planned - first_interest
    second_balance = balance - first_principal
    second_payment = (
        planned if initial == subsequent_rate else monthly_payment(second_balance, subsequent_rate, 2)
    )
    second_interest = interest(second_balance, subsequent_rate, DAYS[1])
    second_principal = second_payment - second_interest
    last_principal = second_balance - second_principal
    return (
        (first_interest, second_interest, interest(last_principal, subsequent_rate, DAYS[2])),
        (first_principal, second_principal, last_principal),
    )


def reference(case):
    """Return source, dated coupon amounts, dated principal amounts and optional history."""
    row = INPUT.copy()
    dates = DATES
    base_coupons = tuple(interest(BALANCE, ".12", days) for days in DAYS)
    coupons = base_coupons
    principal = (D(0), D(0), BALANCE)
    fixings = None

    if case == "rgm-fixed":
        row.update(MONTHLY_BODY, discriminator="RGM")
        part = BALANCE / 3
        principal = (part,) * 3
        coupons = tuple(interest(BALANCE - part * i, ".12", days) for i, days in enumerate(DAYS))
    if case in ("ann-fixed", "ann-final-residual"):
        balance = BALANCE if case == "ann-fixed" else D("123456.78")
        rate = ".12" if case == "ann-fixed" else ".073"
        row.update(MONTHLY_BODY, discriminator="ANN", interest_rate=rate, current_principal_amt=str(balance))
        coupons, principal = expected_annuity(balance, rate, interest(balance, rate, 30), rate)
    if case in ("ann-float-mid-period", "ann-float-on-payment"):
        row.update(MONTHLY_BODY, discriminator="ANN", rate_type_cd="FLOAT",
                   interest_payment="", interest_rate=".02", base_indicator_rate=".08")
        date = "2022-09-15" if case == "ann-float-mid-period" else dates[0]
        row["repricing_final_rate_schedule"] = f"{date},.20"
        first = (
            interest(BALANCE, ".10", 15) + interest(BALANCE, ".20", 15)
            if date.endswith("15") else interest(BALANCE, ".10", 30)
        )
        coupons, principal = expected_annuity(BALANCE, ".10", first, ".20")
    if case in ("accrued-null", "accrued-zero", "accrued-positive"):
        raw = {"accrued-null": None, "accrued-zero": "0", "accrued-positive": "1000"}[case]
        row.update(deal_dttm="2022-08-01", maturity_dttm="2022-10-15",
                   interest_payment="2022-08-01 2022-09-15 2022-10-15")
        if raw is not None:
            row["accrued_interest"] = raw
        dates = ("2022-09-15", "2022-10-15")
        coupons = (interest(BALANCE, ".12", 15) + D(raw or 0), interest(BALANCE, ".12", 30))
        principal = (D(0), BALANCE)
    if case == "float-explicit-final":
        row.update(rate_type_cd="FLOAT", interest_rate=".02", base_indicator_rate=".08",
                   repricing_final_rate_schedule="2022-09-30,.12")
        coupons = (interest(BALANCE, ".10", 30), *base_coupons[1:])
    if case == "historical-fixing":
        row.update(deal_dttm="2022-07-30", rate_type_cd="FLOAT", interest_rate=".02",
                   interest_payment="2022-08-10 " + INPUT["interest_payment"],
                   repricing_dttm_schedule="2022-08-01")
        fixings = "2022-07-31,.08\n2022-08-02,.10\n2022-09-01,.20\n"
        coupons = tuple(interest(BALANCE, ".10", days) for days in DAYS)
    return row, list(zip(dates, coupons, principal)), fixings


CASES = (
    "simple-fixed", "rgm-fixed", "ann-fixed", "ann-float-mid-period",
    "ann-float-on-payment", "accrued-null", "accrued-zero", "accrued-positive",
    "float-explicit-final", "historical-fixing", "ann-final-residual",
)


def read_rows(path, delimiter=","):
    with path.open(encoding="utf-8", newline="") as file:
        return list(csv.DictReader(file, delimiter=delimiter))


def write_source(path, row):
    with path.open("w", encoding="utf-8", newline="") as file:
        writer = csv.DictWriter(file, list(row), delimiter=";")
        writer.writeheader()
        writer.writerow(row)


def run_input(binary, source, out, policy, *options):
    assert main([str(source), "--output-dir", str(out), "--format", "csv",
                 "--cfcore", str(binary), "--policy", policy, "--threads", "1",
                 "--events", "--trace-deal", "46", *options]) == 0
    return out / "core"


@pytest.mark.parametrize("case", CASES)
def test_contractual_v2_against_decimal_reference(binary, tmp_path, case):
    row, expected, history = reference(case)
    source = tmp_path / "bank.csv"
    write_source(source, row)
    options = []
    if history is not None:
        fixings = tmp_path / "fixings.csv"
        fixings.write_text("date,value\n" + history, encoding="utf-8")
        options = ["--fixings", str(fixings)]
    core = run_input(binary, source, tmp_path / "out", "contractual-v2", *options)
    assert read_rows(core / "errors.csv") == []
    manifest = json.loads((core / "run_manifest.json").read_text(encoding="utf-8"))
    assert manifest["calculation_policy"] == "CONTRACTUAL_V2"
    actual = read_rows(core / "cashflows.csv")
    expected_flows = [
        (date, kind, amount) for date, coupon, principal in expected
        for kind, amount in (("INTEREST", coupon), ("PRINCIPAL", principal)) if amount > 0
    ]
    assert [(item["payment_date"], item["cash_flow_type"]) for item in actual] == [
        (date, kind) for date, kind, amount in expected_flows
    ]
    for item, (_, _, amount) in zip(actual, expected_flows, strict=True):
        assert abs(D(item["amount"]) - amount) <= EPSILON
    summary = read_rows(core / "deal_summary.csv")[0]
    assert D(summary["closing_balance"]) == 0
    buckets = read_rows(core / "buckets.csv")
    for key, i in (("interest_cf", 1), ("principal_cf", 2)):
        assert abs(sum(D(bucket[key]) for bucket in buckets) - sum(payment[i] for payment in expected)) <= EPSILON
    trace = read_rows(core / "deal_trace.csv")
    assert D(trace[-1]["balance_after"]) == 0
    events = read_rows(core / "events.csv")
    assert all(event["event_type"] == "REPRICING" and D(event["cash_amount"]) == 0 for event in events)
    if case == "ann-float-on-payment":
        assert [item["event_type"] for item in trace if item["event_date"] == DATES[0]] == [
            "INTEREST", "PRINCIPAL", "REPRICING"
        ]
    if case == "historical-fixing":
        assert trace[0]["last_repricing_date"] == "2022-08-01"
        assert trace[0]["rate_source_before"] == "FIXING_HISTORY"


def test_wire_version_keeps_v1_and_requires_opt_in_for_v2_fields(binary, tmp_path):
    source = tmp_path / "portfolio.csv"
    write_source(source, INPUT)
    base = run_input(binary, source, tmp_path / "base", "base")
    v1 = base.parent / "normalized_deals.csv"
    assert tuple(read_rows(v1, ";")[0]) == OUTPUT_COLUMNS
    compatible = tmp_path / "v1-in-v2"
    subprocess.run([binary, "--input", str(v1), "--output-dir", str(compatible),
                    "--policy", "contractual-v2", "--threads", "1"], check=True)
    assert (base / "cashflows.csv").read_bytes() == (compatible / "cashflows.csv").read_bytes()

    row, _, _ = reference("float-explicit-final")
    write_source(source, row | {"indicator_lag_days": "0"})
    v2 = run_input(binary, source, tmp_path / "v2", "contractual-v2").parent / "normalized_deals.csv"
    mapped = read_rows(v2, ";")[0]
    assert tuple(mapped) == OUTPUT_COLUMNS_V2
    assert mapped["indicator_lag_days"] == "0"
    assert mapped["repricing_final_rate_schedule"] == "2022-09-30,0.12"
    mismatched = tmp_path / "v2-in-base"
    subprocess.run([binary, "--input", str(v2), "--output-dir", str(mismatched),
                    "--policy", "base", "--threads", "1"], check=True)
    assert read_rows(mismatched / "errors.csv") == [{"instrument_id": "46", "error_code": "UNSUPPORTED"}]
    assert read_rows(mismatched / "cashflows.csv") == []


def test_deal_46_requires_confirmation_before_migration(binary, tmp_path):
    sample = ROOT / "data/input/test_deals_mai.csv"
    fixings = ROOT / "data/input/key_rate.csv"
    options = ["--fixings", str(fixings)]
    base = run_input(binary, sample, tmp_path / "base", "base", *options)
    row = next(item for item in read_rows(base.parent / "normalized_deals.csv", ";") if item["deal_id"] == "46")
    assert row["interest_rate"] == "0.05"
    assert row["base_indicator_rate"] == ""
    assert row["repricing_dttm_rate_schedule_fixed"] == "2022-08-31,0.05"
    first = next(item for item in read_rows(base / "cashflows.csv") if item["instrument_id"] == "46")
    assert first["payment_date"] == "2022-09-30"
    assert D(first["rate_used"]) == D(".13")
    assert abs(D(first["amount"]) - interest(D("156750"), ".13", 30)) <= EPSILON
    v2 = run_input(binary, sample, tmp_path / "v2", "contractual-v2", *options)
    assert {"deal_id": "46", "reason_code": "AMBIGUOUS_RATE_SCHEDULE"} in read_rows(v2.parent / "quarantine.csv")
    assert all(item["instrument_id"] != "46" for item in read_rows(v2 / "cashflows.csv"))
    assert len(read_rows(v2 / "deal_summary.csv")) == 43
