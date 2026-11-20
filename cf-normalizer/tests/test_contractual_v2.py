"""Numerical cross-layer regressions for the opt-in CONTRACTUAL_V2 policy."""

import csv
import json
import os
from pathlib import Path

import pytest

from normalizer.__main__ import main


# Amount tolerances are in RUB. Rates and year fractions use a separate tolerance.
MONEY = 1e-7
RATE = 1e-10
TERM = "2022-11-30"
BANK = {
    "deal_id": "701", "report_dttm": "2022-08-31", "deal_dttm": "2022-08-01",
    "maturity_dttm": TERM, "current_principal_amt": "100000", "currency_id": "RUB",
    "discriminator": "SIMPLE", "rate_type_cd": "FIXED", "interest_rate": "0.12",
    "day_convention_cd": "ACT/365", "interest_payment": "2022-08-01 2022-09-15 2022-10-15 2022-11-30",
}


@pytest.fixture
def binary():
    path = os.environ.get("CFCORE_BIN")
    if not path:
        pytest.skip("CFCORE_BIN required")
    return Path(path)


def read_rows(path, delimiter=","):
    with path.open(encoding="utf-8", newline="") as file:
        return list(csv.DictReader(file, delimiter=delimiter))


def run_bank(tmp_path, binary, *deals, history=None, out_name="out"):
    source = tmp_path / f"{out_name}.csv"
    names = list(dict.fromkeys(key for row in deals for key in row))
    with source.open("w", encoding="utf-8", newline="") as file:
        writer = csv.DictWriter(file, names, delimiter=";")
        writer.writeheader()
        writer.writerows(deals)
    out = tmp_path / out_name
    args = [str(source), "--output-dir", str(out), "--cfcore", str(binary), "--format", "csv",
            "--policy", "contractual-v2", "--events", "--trace-deal", "701", "--threads", "1"]
    if history is not None:
        fixing_path = tmp_path / f"{out_name}-fixings.csv"
        fixing_path.write_text("date,value\n" + history, encoding="utf-8")
        args.extend(["--fixings", str(fixing_path)])
    assert main(args) == 0
    assert json.loads((out / "core/run_manifest.json").read_text(encoding="utf-8"))["calculation_policy"] == "CONTRACTUAL_V2"
    return out


def coupon(flows, date):
    return sum(float(row["amount"]) for row in flows if row["payment_date"] == date and row["cash_flow_type"] == "INTEREST")


def principal(flows, date):
    return sum(float(row["amount"]) for row in flows if row["payment_date"] == date and row["cash_flow_type"] == "PRINCIPAL")


def pmt(balance, annual_rate, n):
    monthly = annual_rate / 12
    return balance * monthly / (1 - (1 + monthly) ** -n)


@pytest.mark.parametrize("raw", [None, "", "0", "1000"])
def test_first_coupon_from_report_and_carry_once(tmp_path, binary, raw):
    row = BANK if raw is None else BANK | {"accrued_interest": raw}
    out = run_bank(tmp_path, binary, row)
    flows = read_rows(out / "core/cashflows.csv")
    assert coupon(flows, "2022-09-15") == pytest.approx(100000 * .12 * 15 / 365 + float(raw or 0), abs=MONEY, rel=0)
    assert coupon(flows, "2022-10-15") == pytest.approx(100000 * .12 * 30 / 365, abs=MONEY, rel=0)
    summary = read_rows(out / "core/deal_summary.csv")[0]
    assert summary["current_interest_source"] == ("REPORTED" if raw else "UNAVAILABLE")
    first = read_rows(out / "core/deal_trace.csv")[0]
    assert first["period_start"] == "2022-08-31"
    assert first["accrued_interest_included"] == (raw or "")
    assert float(first["balance_before"]) == float(first["balance_after"]) == 100000


def test_negative_accrued_is_rejected(tmp_path, binary):
    out = run_bank(tmp_path, binary, BANK | {"accrued_interest": "-1"})
    assert read_rows(out / "core/errors.csv") == [{"instrument_id": "701", "error_code": "INVALID_NUMBER"}]


def test_final_rate_priority_split_and_resume_index_rule(tmp_path, binary):
    row = BANK | {"rate_type_cd": "FLOAT", "interest_rate": ".02", "base_indicator_rate": ".08",
                  "repricing_dttm_schedule": "2022-09-05 2022-10-01",
                  "repricing_final_rate_schedule": "2022-09-05,.12 2022-09-05,.12",
                  "cap_limit_interest_rate": ".115", "accrued_interest": "1000"}
    core = run_bank(tmp_path, binary, row) / "core"
    flows = read_rows(core / "cashflows.csv")
    assert coupon(flows, "2022-09-15") == pytest.approx(1000 + 100000 * (.10 * 5 + .115 * 10) / 365, abs=MONEY, rel=0)
    assert coupon(flows, "2022-10-15") == pytest.approx(100000 * (.115 * 16 + .10 * 14) / 365, abs=MONEY, rel=0)
    events = read_rows(core / "events.csv")
    assert [event["rate_source"] for event in events] == ["CONTRACT_SCHEDULE", "DEAL_INDICATOR"]
    assert [float(event["rate_after"]) for event in events] == pytest.approx([.115, .10], abs=RATE, rel=0)
    assert [float(event["cash_amount"]) for event in events] == [0, 0]
    trace = read_rows(core / "deal_trace.csv")
    assert trace[1]["rate_source_before"] == "CONTRACT_SCHEDULE"
    assert trace[1]["last_repricing_date"] == "2022-09-05"


@pytest.mark.parametrize("extras,code", [
    ({"repricing_final_rate_schedule": "2022-09-05,.12 2022-09-05,.13"}, "CONFLICTING_REPRICING_RATES"),
    ({"repricing_dttm_rate_schedule_fixed": "2022-09-05,.12"}, "AMBIGUOUS_RATE_SCHEDULE"),
    ({"indicator_lag_cd": "-1M"}, "UNSUPPORTED_INDICATOR_LAG"),
    ({"indicator_lag_days": "-2"}, "INVALID_INDICATOR_LAG"),
    ({"repricing_final_rate_schedule": "2022-12-01,.12"}, "INVALID_SCHEDULE"),
    ({"repricing_final_rate_schedule": "2022-11-30,.12 2022-11-30,.13"}, "CONFLICTING_REPRICING_RATES"),
])
def test_input_errors_are_explicit(tmp_path, binary, extras, code):
    row = BANK | {"rate_type_cd": "FLOAT", "interest_rate": ".02", "base_indicator_rate": ".08"} | extras
    out = run_bank(tmp_path, binary, row)
    errors = read_rows(out / "core/errors.csv")
    rejected = read_rows(out / "quarantine.csv")
    assert [item.get("error_code", item.get("reason_code")) for item in errors + rejected] == [code]


ANN = BANK | {"discriminator": "ANN", "rate_type_cd": "FLOAT", "interest_rate": ".02",
              "base_indicator_rate": ".08", "interest_payment": "",
              "regular_payment_rule_start_dttm": "2022-08-31", "regular_payment_rule_period_cd": "1M",
              "regular_payment_rule_day_num": "31"}


@pytest.mark.parametrize("first_reset", ["2022-09-15", "2022-09-30"])
def test_ann_defers_pmt_and_closes_after_multiple_resets(tmp_path, binary, first_reset):
    row = ANN | {"repricing_final_rate_schedule": f"{first_reset},.20 2022-10-10,.22 2022-10-20,.24 2022-11-29,.26"}
    core = run_bank(tmp_path, binary, row) / "core"
    flows = read_rows(core / "cashflows.csv")
    first_coupon = 100000 * (.10 * 15 + .20 * 15 if first_reset.endswith("15") else .10 * 30) / 365
    assert coupon(flows, "2022-09-30") == pytest.approx(first_coupon, abs=MONEY, rel=0)
    old_payment = pmt(100000, .10, 3)
    first_principal = principal(flows, "2022-09-30")
    assert first_principal == pytest.approx(old_payment - first_coupon, abs=MONEY, rel=0)
    balance = 100000 - first_principal
    next_coupon = balance * (.20 * 10 + .22 * 10 + .24 * 11) / 365
    assert coupon(flows, "2022-10-31") == pytest.approx(next_coupon, abs=MONEY, rel=0)
    assert principal(flows, "2022-10-31") == pytest.approx(pmt(balance, .20, 2) - next_coupon, abs=MONEY, rel=0)
    principal_rows = [float(row["amount"]) for row in flows if row["cash_flow_type"] == "PRINCIPAL"]
    assert all(0 < paid <= 100000 for paid in principal_rows)
    assert sum(principal_rows) == pytest.approx(100000, abs=MONEY, rel=0)
    trace = read_rows(core / "deal_trace.csv")
    september = [row for row in trace if row["event_date"] == "2022-09-30"]
    assert [row["event_type"] for row in september] == (
        ["INTEREST", "PRINCIPAL", "REPRICING"] if first_reset.endswith("30") else ["INTEREST", "PRINCIPAL"])
    assert float(september[-1]["pmt_before"]) == pytest.approx(old_payment, abs=MONEY, rel=0)
    assert float(september[-1]["pmt_after"]) == pytest.approx(pmt(balance, .20, 2), abs=MONEY, rel=0)
    assert float(trace[-1]["balance_after"]) == 0


def test_historical_periodic_fixing_with_calendar_lag_sets_first_ann_pmt(tmp_path, binary):
    row = ANN | {"repricing_rule_start_dttm": "2022-08-20", "repricing_rule_period_cd": "3M",
                 "indicator_lag_days": "2"}
    core = run_bank(tmp_path, binary, row,
                    history="2022-08-17,0.08\n2022-08-19,0.14\n2022-08-30,0.18\n2022-09-01,0.22\n") / "core"
    flows = read_rows(core / "cashflows.csv")
    assert coupon(flows, "2022-09-30") == pytest.approx(100000 * .10 * 30 / 365, abs=MONEY, rel=0)
    assert principal(flows, "2022-09-30") == pytest.approx(pmt(100000, .10, 3) - 100000 * .10 * 30 / 365, abs=MONEY, rel=0)
    trace = read_rows(core / "deal_trace.csv")
    assert trace[0]["last_repricing_date"] == "2022-08-20"
    assert trace[0]["rate_source_before"] == "FIXING_HISTORY"
    november_reset = read_rows(core / "events.csv")[0]
    assert november_reset["event_date"] == "2022-11-20"
    # The September fixing was unavailable on the valuation date.
    assert float(november_reset["rate_after"]) == pytest.approx(.18 + .02, abs=RATE, rel=0)


def test_missing_prior_fixing_and_payment_below_coupon(tmp_path, binary):
    historical = ANN | {"repricing_dttm_schedule": "2022-08-20"}
    out = run_bank(tmp_path, binary, historical, history="2022-08-21,0.08\n")
    assert read_rows(out / "core/errors.csv")[0]["error_code"] == "MISSING_RATE"
    out = run_bank(tmp_path, binary, ANN | {"accrued_interest": "100000"}, out_name="large-coupon")
    assert read_rows(out / "core/errors.csv")[0]["error_code"] == "INTEREST_EXCEEDS_ANNUITY"
    assert read_rows(out / "core/cashflows.csv") == []


def test_manual_json_policy_matches_portfolio(tmp_path, binary):
    manual = tmp_path / "manual.json"
    manual.write_text(json.dumps({
        "instrument_id": 701, "reporting_date": "2022-08-31", "start_date": "2022-08-01",
        "maturity_date": TERM, "outstanding_principal": 100000, "currency": "RUB",
        "amortization": "SIMPLE", "rate_type": "FLOAT", "day_count": "ACT/365", "spread": .02,
        "base_indicator_rate": .08, "policy": "contractual-v2", "accrued_interest": 1000,
        "interest_payment_dates": BANK["interest_payment"].split(),
        "repricing_final_rates": [{"date": "2022-09-05", "rate": .12}],
    }), encoding="utf-8")
    manual_out = tmp_path / "manual"
    assert main([str(manual), "--manual", "--output-dir", str(manual_out),
                 "--cfcore", str(binary), "--format", "csv"]) == 0
    batch = run_bank(tmp_path, binary, BANK | {
        "rate_type_cd": "FLOAT", "interest_rate": ".02", "base_indicator_rate": ".08",
        "repricing_final_rate_schedule": "2022-09-05,.12", "accrued_interest": "1000",
    })
    for name in ("cashflows.csv", "buckets.csv", "deal_summary.csv", "events.csv", "deal_trace.csv"):
        assert read_rows(batch / "core" / name) == read_rows(manual_out / "core" / name)


def test_pending_annuity_and_same_day_reset_use_one_new_pmt(tmp_path, binary):
    row = ANN | {"repricing_final_rate_schedule": "2022-09-15,.20 2022-09-30,.24"}
    core = run_bank(tmp_path, binary, row) / "core"
    rows = [row for row in read_rows(core / "deal_trace.csv") if row["event_date"] == "2022-09-30"]
    assert len(rows) == 3
    expected = pmt(100000, .10, 3)
    assert float(rows[1]["pmt_before"]) == float(rows[1]["pmt_after"])
    assert float(rows[2]["pmt_before"]) == pytest.approx(expected, abs=MONEY, rel=0)
    assert float(rows[2]["pmt_after"]) == pytest.approx(
        pmt(float(rows[1]["balance_after"]), .24, 2), abs=MONEY, rel=0)
