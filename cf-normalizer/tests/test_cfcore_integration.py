"""INP-4: необязательный регрессионный прогон настоящего C++-ядра.

CFCORE_BIN указывает на собранное ядро. История ставок берётся из
репозитория, если не указан CFCORE_FIXINGS.
"""

import csv
import hashlib
import json
import os
from pathlib import Path

import pytest

from normalizer.__main__ import main


ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = ROOT.parent
SAMPLE = REPO_ROOT / "data/input/test_deals_mai.csv"
DEFAULT_FIXINGS = REPO_ROOT / "data/input/key_rate.csv"
GOLDEN = ROOT / "tests/golden"
FIXINGS_SHA256 = "8b02d6c5b037689c5563addfb55af116e0a26da8ad89281924875fff935a5c5a"


def _read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as file:
        return list(csv.DictReader(file))


def _fixings_digest(content: bytes) -> str:
    # Нормализуем только границы строк. Содержимое и порядок остаются в хеше.
    return hashlib.sha256(content.replace(b"\r\n", b"\n")).hexdigest()


def test_fixings_digest_is_stable_and_checks_content():
    data = DEFAULT_FIXINGS.read_bytes().replace(b"\r\n", b"\n")
    assert _fixings_digest(data) == _fixings_digest(data.replace(b"\n", b"\r\n")) == FIXINGS_SHA256
    assert _fixings_digest(data.replace(b",", b";", 1)) != FIXINGS_SHA256


@pytest.fixture
def cfcore():
    binary = os.environ.get("CFCORE_BIN")
    if not binary:
        pytest.skip("Для проверки настоящего ядра задайте CFCORE_BIN")
    path = Path(binary).resolve()
    assert path.is_file(), f"Нет исполняемого C++-ядра: {path}"
    return path


def test_pinned_cfcore_on_all_55_deals(tmp_path, cfcore):
    fixings = Path(os.environ.get("CFCORE_FIXINGS", DEFAULT_FIXINGS)).resolve()
    assert fixings.is_file(), f"Нет файла исторических ставок: {fixings}"
    assert _fixings_digest(fixings.read_bytes()) == FIXINGS_SHA256, (
        "История ставок отличается от зафиксированной в INP-4 (data/input/key_rate.csv)"
    )

    out = tmp_path / "result"
    assert main([
        str(SAMPLE), "--output-dir", str(out), "--format", "both",
        "--cfcore", str(cfcore), "--fixings", str(fixings), "--threads", "1",
    ]) == 0

    with (out / "normalized_deals.csv").open(encoding="utf-8", newline="") as file:
        assert len(list(csv.DictReader(file, delimiter=";"))) == 45
    assert len(_read_csv(out / "quarantine.csv")) == 10
    core = out / "core"
    assert not (core / "events.csv").exists()
    assert not (core / "deal_trace.csv").exists()
    assert _read_csv(core / "errors.csv") == _read_csv(GOLDEN / "core_errors.csv") == [
        {"instrument_id": "20", "error_code": "SCHEDULE_SUM_MISMATCH"}
    ]

    expected_summary = {row["instrument_id"]: row for row in _read_csv(GOLDEN / "core_deal_summary.csv")}
    actual_summary = {row["instrument_id"]: row for row in _read_csv(core / "deal_summary.csv")}
    assert actual_summary.keys() == expected_summary.keys()
    assert len(actual_summary) == 44
    for deal_id, expected in expected_summary.items():
        actual = actual_summary[deal_id]
        assert actual.keys() == expected.keys()
        assert actual["flow_count"] == expected["flow_count"], deal_id
        assert actual["current_interest_source"] == expected["current_interest_source"], deal_id
        for name in ("opening_principal", "total_principal", "total_interest",
                     "closing_balance", "current_interest"):
            assert float(actual[name]) == pytest.approx(
                float(expected[name]), rel=1e-9, abs=0.02
            ), (deal_id, name)

    with (core / "cashflows.csv").open(encoding="utf-8", newline="") as stream:
        flows = list(csv.DictReader(stream))
    assert len(flows) == 3965
    assert sum(int(row["flow_count"]) for row in actual_summary.values()) == len(flows)

    manifest = json.loads((core / "run_manifest.json").read_text(encoding="utf-8"))
    assert manifest["mode"] == "BASE"
    assert manifest["deals_ok"] == 44
    assert manifest["deals_failed"] == 1
    assert manifest["flows"] == 3965


EXAMPLES = REPO_ROOT / "examples/manual"
BANK_DEAL = {
    "deal_id": 105,
    "report_dttm": "2022-08-31",
    "deal_dttm": "2022-08-31",
    "maturity_dttm": "2022-10-31",
    "current_principal_amt": 100000,
    "currency_id": "RUB",
    "discriminator": "SIMPLE",
    "rate_type_cd": "FIXED",
    "interest_rate": "0.10",
    "day_convention_cd": "ACT/365",
    "interest_payment": "2022-09-30 2022-10-31",
    "principal_payment": "2022-09-10,30000 2022-10-01,70000",
}


def _bank_csv(path, *rows):
    columns = list(dict.fromkeys(key for row in rows for key in row))
    with path.open("w", encoding="utf-8", newline="") as file:
        writer = csv.DictWriter(file, columns, delimiter=";")
        writer.writeheader()
        writer.writerows(rows)


def _run_deals(cfcore, source, out, *options):
    return main([str(source), "--output-dir", str(out), "--format", "csv",
                 "--cfcore", str(cfcore), "--threads", "1", *options])


def _amounts(flows, kind):
    return [float(row["amount"]) for row in flows if row["cash_flow_type"] == kind]


@pytest.mark.parametrize("example", ["simple-fixed", "rgm-fixed", "ann-fixed", "ann-float", "simple-irregular"])
def test_manual_examples(tmp_path, cfcore, example):
    out = tmp_path / "Ручная сделка"
    source = EXAMPLES / f"{example}.json"
    expected_id = str(json.loads(source.read_text(encoding="utf-8"))["instrument_id"])
    assert _run_deals(cfcore, source, out, "--manual") == 0
    core = out / "core"
    manifest = json.loads((core / "run_manifest.json").read_text(encoding="utf-8"))
    assert manifest["deals_ok"] == 1 and manifest["deals_failed"] == 0
    flows = _read_csv(core / "cashflows.csv")
    trace = _read_csv(core / "deal_trace.csv")
    assert {row["instrument_id"] for row in flows + trace} == {expected_id}
    principal = _amounts(flows, "PRINCIPAL")
    interest = _amounts(flows, "INTEREST")
    events = _read_csv(core / "events.csv")

    assert sum(principal) == pytest.approx(100000)
    assert float(trace[-1]["balance_after"]) == 0
    assert sum(float(row["interest_paid"]) for row in trace) == pytest.approx(sum(interest))
    if example == "simple-fixed":
        assert principal == [100000]
        assert flows[-1]["payment_date"] == "2022-11-30"
    if example == "simple-irregular":
        assert principal == [30000, 70000]
        assert interest[0] == pytest.approx((100000 * 10 + 70000 * 20) * 0.10 / 365)
    if example == "rgm-fixed":
        assert principal == pytest.approx([100000 / 3] * 3)
        assert interest[0] > interest[1] > interest[2] > 0
    if example == "ann-fixed":
        assert principal[0] + interest[0] == pytest.approx(principal[1] + interest[1])
    if example != "ann-float":
        assert events == []
        return

    assert [row["event_date"] for row in events] == ["2022-09-15", "2022-10-31"]
    assert [float(row["rate_after"]) for row in events] == pytest.approx([0.20, 0.12])
    assert all(float(row["cash_amount"]) == 0 for row in events)
    assert [(int(row["days"]), float(row["rate_before"])) for row in trace[:2]] == [(15, 0.10), (15, 0.20)]
    assert interest[0] == pytest.approx(100000 * (0.10 + 0.20) * 15 / 365)
    assert [row["event_type"] for row in trace if row["event_date"] == "2022-10-31"] == [
        "INTEREST", "PRINCIPAL", "REPRICING"
    ]
    current_rate = 0.20 / 12
    working_rule_rate = 0.10 / 12
    actual_payment = principal[0] + interest[0]
    assert actual_payment == pytest.approx(100000 * current_rate / (1 - (1 + current_rate) ** -3))
    assert actual_payment != pytest.approx(100000 * working_rule_rate / (1 - (1 + working_rule_rate) ** -3))


def test_manual_matches_deal_in_bank_portfolio(tmp_path, cfcore):
    source = tmp_path / "Банк.csv"
    _bank_csv(source, BANK_DEAL, BANK_DEAL | {"deal_id": 900, "product": "Другой портфель"})
    manual = tmp_path / "manual"
    bank = tmp_path / "bank"
    assert _run_deals(cfcore, EXAMPLES / "simple-irregular.json", manual, "--manual") == 0
    assert _run_deals(cfcore, source, bank, "--events", "--trace-deal", "105") == 0
    for name in ("cashflows.csv", "deal_summary.csv", "deal_trace.csv"):
        rows = _read_csv(bank / "core" / name)
        assert [row for row in rows if row["instrument_id"] == "105"] == _read_csv(manual / "core" / name)
    bucket_rows = _read_csv(bank / "core/buckets.csv")
    assert [row for row in bucket_rows if row["portfolio_key"] == "0"] == _read_csv(manual / "core/buckets.csv")



def test_prepayment_manual_and_bank_agree(tmp_path, cfcore):
    source = tmp_path / "Банк.csv"
    _bank_csv(source, BANK_DEAL, BANK_DEAL | {"deal_id": 900, "product": "Другой портфель"})
    eligible = tmp_path / "Портфели.csv"
    eligible.write_text("portfolio_key,recast\n0,proportional\n", encoding="utf-8")
    options = ("--prepayment-model", "bvdpk", "--prepayment-eligible", str(eligible), "--bvdpk", "0.02")
    manual, bank = tmp_path / "manual", tmp_path / "bank"
    assert _run_deals(cfcore, EXAMPLES / "simple-irregular.json", manual, "--manual", *options) == 0
    assert _run_deals(cfcore, source, bank, *options) == 0
    buckets = _read_csv(bank / "core/behavioral_buckets.csv")
    assert buckets == _read_csv(manual / "core/behavioral_buckets.csv")
    assert {r["portfolio_key"] for r in buckets} == {"0"}
    assert sum(float(r["principal_cf"]) for r in buckets) == pytest.approx(100000)
    assert buckets[1]["scheduled_principal_cf"] == "30000"
    assert buckets[1]["prepayment_cf"] == "2000"
    assert buckets[1]["closing_balance"] == "68000"
    assert float(buckets[2]["interest_cf"]) == pytest.approx(68000 * 0.10 / 365)
    assert (manual / "core/deal_trace.csv").exists()
    assert {row["instrument_id"] for row in _read_csv(bank / "core/cashflows.csv")} == {"105", "900"}


def test_irrbb_portfolio_pipeline(tmp_path, cfcore):
    source = tmp_path / "bank.csv"
    _bank_csv(source, BANK_DEAL | {"rate_type_cd": "FLOAT", "base_indicator_rate": "0.08",
                                  "interest_rate": "0.02", "repricing_dttm_schedule": "2022-10-15"})
    coupons = tmp_path / "Coupons.csv"
    coupons.write_text("instrument_id,payment_date,known_interest\n105,2022-09-30,500\n105,2022-10-31,\n",
                       encoding="utf-8")
    output = tmp_path / "out"
    assert _run_deals(cfcore, source, output, "--risk-buckets", "8mr", "--risk-coupons", str(coupons)) == 0
    allocations = _read_csv(output / "core/risk_bucket_allocations.csv")
    principal = [row for row in allocations if row["flow_type"] == "PRINCIPAL"]
    assert {(row["payment_date"], row["risk_date"]) for row in principal} == {
        ("2022-09-10", "2022-09-10"), ("2022-10-01", "2022-10-01"),
    }
    interest = {row["coupon_status"]: row for row in allocations if row["flow_type"] == "INTEREST"}
    assert interest["KNOWN"]["risk_date"] == "2022-10-15"
    assert float(interest["KNOWN"]["risk_amount"]) == 500
    assert float(interest["UNKNOWN"]["risk_amount"]) == pytest.approx(70000 * 0.02 / 365)


def test_combined_diagnostics_without_cashflows(tmp_path, cfcore):
    source = tmp_path / "deals.csv"
    _bank_csv(source, BANK_DEAL | {"repricing_dttm_rate_schedule_fixed": "2022-09-30,0.20"})
    assert _run_deals(cfcore, source, tmp_path / "out", "--events", "--trace-deal", "105", "--no-flows") == 0
    core = tmp_path / "out/core"
    assert not (core / "cashflows.csv").exists()
    assert _read_csv(core / "events.csv")[0]["event_type"] == "REPRICING"
    assert _read_csv(core / "deal_trace.csv")
    assert _read_csv(core / "buckets.csv")
    assert json.loads((core / "run_manifest.json").read_text(encoding="utf-8"))["flows"] > 0


@pytest.mark.parametrize("state", ["missing", "empty", "zero", "reported"])
@pytest.mark.parametrize("excel", [False, True], ids=["base", "excel"])
def test_first_coupon_current_vs_working_rule(tmp_path, cfcore, state, excel):
    row = BANK_DEAL | {"deal_dttm": "2022-08-01", "interest_rate": "0.12", "principal_payment": "",
                       "interest_payment": "2022-08-01 2022-09-30 2022-10-31"}
    input_values = {"empty": "", "zero": "0", "reported": "123.45"}
    if state != "missing":
        row["accrued_interest"] = input_values[state]
    source = tmp_path / "interest.csv"
    _bank_csv(source, row)
    out = tmp_path / "out"
    assert _run_deals(cfcore, source, out, *(["--excel-etalon"] if excel else [])) == 0
    with (out / "normalized_deals.csv").open(encoding="utf-8", newline="") as file:
        normalized = next(csv.DictReader(file, delimiter=";"))
    assert normalized["accrued_interest"] == input_values.get(state, "")
    current = _read_csv(out / "core/deal_summary.csv")[0]
    assert current["current_interest_source"] == ("REPORTED" if state in ("zero", "reported") else "COMPUTED")
    post_report = 100000 * 0.12 * 30 / 365
    assert float(current["current_interest"]) == pytest.approx({
        "zero": 0, "reported": 123.45,
    }.get(state, post_report))
    first = _amounts(_read_csv(out / "core/cashflows.csv"), "INTEREST")[0]
    assert first == pytest.approx(post_report * 2)
    proposed_coupon = post_report + (123.45 if state == "reported" else 0)
    assert first != pytest.approx(proposed_coupon)


def test_manual_kernel_rejection_is_reported(tmp_path, cfcore):
    source = tmp_path / "bad.json"
    deal = json.loads((EXAMPLES / "simple-irregular.json").read_text(encoding="utf-8"))
    deal["principal_payments"][-1]["amount"] = 50000
    source.write_text(json.dumps(deal), encoding="utf-8")
    out = tmp_path / "out"
    assert _run_deals(cfcore, source, out, "--manual") == 2
    assert _read_csv(out / "core/errors.csv") == [{"instrument_id": "105", "error_code": "SCHEDULE_ENDS_EARLY"}]
