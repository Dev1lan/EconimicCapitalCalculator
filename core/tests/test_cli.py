"""End-to-end checks for the native cfcore executable."""

import csv
import json
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import unittest


COLUMNS = runpy.run_path(str(Path(__file__).resolve().parents[2] / "tools/normalized_format.py"))["COLUMNS"]
FINANCIAL_OUTPUT = ("cashflows.csv", "buckets.csv", "deal_summary.csv", "errors.csv")


def deal(instrument_id, **overrides):
    return {
        "deal_id": instrument_id,
        "report_dttm": "2022-08-31",
        "discriminator": "SIMPLE",
        "currency_id": "RUB",
        "side": "ASSET",
        "portfolio_key": 0,
        "current_principal_amt": 100000,
        "deal_dttm": "2022-08-31",
        "maturity_dttm": "2022-10-31",
        "rate_type_cd": "FIXED",
        "interest_rate": 0.10,
        "day_convention_cd": "ACT_365",
        "interest_payment": "2022-09-30 2022-10-31",
        **overrides,
    }


def rows(path):
    with path.open(encoding="utf-8", newline="") as file:
        return list(csv.DictReader(file))


class Cli(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.folder = Path(temporary.name)

    def write_deals(self, path, *deals):
        with path.open("w", encoding="utf-8", newline="") as file:
            writer = csv.DictWriter(file, COLUMNS, delimiter=";")
            writer.writeheader()
            writer.writerows(deals)

    def run_cfcore(self, input_path, output_dir, *args):
        result = subprocess.run(
            [BINARY, "--input", str(input_path), "--output-dir", str(output_dir), "--threads", "1", *args],
            capture_output=True, encoding="utf-8", errors="replace", check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def manifest(self, output_dir):
        return json.loads((output_dir / "run_manifest.json").read_text(encoding="utf-8"))


    def test_irrbb_allocations(self):
        input_path = self.folder / "deals.csv"
        output = self.folder / "risk"
        baseline = self.folder / "baseline"
        self.write_deals(
            input_path,
            deal(1),
            deal(2, rate_type_cd="FLOAT", base_indicator_rate=0.08, interest_rate=-0.02,
                 maturity_dttm="2023-08-31", principal_payment=(
                     "2022-09-30,10000 2022-11-30,20000 2023-08-31,70000"),
                 interest_payment="2022-09-30 2022-12-31 2023-08-31",
                 repricing_dttm_schedule="2022-11-30 2023-02-28"),
        )
        coupons = self.folder / "Купоны.csv"
        coupons.write_text(
            "instrument_id,payment_date,known_interest\n"
            "2,2022-09-30,500\n2,2022-12-31,\n2,2023-08-31,\n", encoding="utf-8",
        )
        self.run_cfcore(input_path, baseline, "--events", "--trace-deal", "2")
        self.run_cfcore(input_path, output, "--events", "--trace-deal", "2",
                        "--risk-buckets", "8mr", "--risk-coupons", str(coupons))
        for name in (*FINANCIAL_OUTPUT, "events.csv", "deal_trace.csv"):
            self.assertEqual((output / name).read_bytes(), (baseline / name).read_bytes(), name)
        risk = rows(output / "risk_bucket_allocations.csv")
        self.assertEqual([row["risk_date"] for row in risk if row["instrument_id"] == "1"],
                         [row["payment_date"] for row in risk if row["instrument_id"] == "1"])
        floating = {(row["payment_date"], row["flow_type"]): row for row in risk
                    if row["instrument_id"] == "2"}
        for date, risk_date, bucket, amount in (
            ("2022-09-30", "2022-09-30", "1", 10000),
            ("2022-11-30", "2022-11-30", "2", 20000),
            ("2023-08-31", "2022-11-30", "2", 70000),
        ):
            row = floating[date, "PRINCIPAL"]
            self.assertEqual((row["next_planned_repricing"], row["risk_date"], row["bucket"]),
                             ("2022-11-30", risk_date, bucket))
            self.assertEqual(float(row["risk_amount"]), amount)
        known = floating["2022-09-30", "INTEREST"]
        self.assertEqual((known["risk_date"], known["coupon_status"], float(known["risk_amount"])),
                         ("2022-11-30", "KNOWN", 500))
        unknown = floating["2022-12-31", "INTEREST"]
        self.assertEqual((unknown["risk_date"], unknown["coupon_status"]), ("2022-12-31", "UNKNOWN"))
        self.assertAlmostEqual(float(unknown["risk_amount"]), -0.02 * (90000 * 61 + 70000 * 31) / 365)
        buckets = rows(output / "risk_buckets.csv")
        self.assertEqual(len(buckets), 19)
        for row in buckets:
            for kind, column in (("PRINCIPAL", "principal_cf"), ("INTEREST", "interest_cf")):
                self.assertAlmostEqual(float(row[column]), sum(
                    float(a["risk_amount"]) for a in risk
                    if a["bucket"] == row["bucket"] and a["flow_type"] == kind))

    def test_irrbb_incomplete_diagnostics(self):
        path = self.folder / "deals.csv"
        coupons = self.folder / "coupons.csv"
        coupons.write_text(
            "instrument_id,payment_date,known_interest\n"
            "3,2022-09-30,\n3,2022-10-31,\n4,2022-09-30,\n4,2022-10-31,\n", encoding="utf-8",
        )
        self.write_deals(
            path,
            deal(1, rate_type_cd="FLOAT", base_indicator_rate=0.08),
            deal(2, rate_type_cd="FLOAT", base_indicator_rate=0.08, repricing_dttm_schedule="2022-09-15"),
            deal(3, rate_type_cd="FLOAT", base_indicator_rate=0.08, repricing_dttm_schedule="2022-09-15",
                 cap_limit_interest_rate=0.20),
            deal(4, rate_type_cd="FLOAT", base_indicator_rate=0.08, repricing_dttm_schedule="2022-11-15"),
        )
        out = self.folder / "risk"
        result = subprocess.run(
            [BINARY, "--input", str(path), "--output-dir", str(out), "--risk-buckets", "8mr",
             "--risk-coupons", str(coupons)], capture_output=True, check=False,
        )
        self.assertEqual(result.returncode, 3, result.stderr)
        diagnostics = rows(out / "risk_bucket_allocations.csv")
        self.assertTrue({"MISSING_PLANNED_REPRICING", "MISSING_COUPON_STATUS", "PREMIUM_UNRESOLVED",
                         "INVALID_PLANNED_REPRICING"}.issubset({row["rule"] for row in diagnostics}))
        self.assertFalse(self.manifest(out)["risk_buckets"]["complete"])
        self.assertFalse((out / "risk_buckets.csv").exists())

    def test_unicode_paths_and_manifest(self):
        input_dir = self.folder / "Входные данные"
        input_dir.mkdir()
        input_path = input_dir / "Сделка.csv"
        fixings = input_dir / "История ставок.csv"
        fixings.write_text("date,value\n2022-08-31,0.08\n", encoding="utf-8")
        self.write_deals(input_path, deal(1, rate_type_cd="FLOAT", interest_rate=0.02))
        output = self.folder / "Отчёты по сделкам"

        self.run_cfcore(input_path, output, "--fixings", str(fixings))

        manifest = self.manifest(output)
        self.assertEqual(manifest["input"], str(input_path))
        self.assertEqual(manifest["deals_ok"], 1)
        self.assertEqual(manifest["deals_failed"], 0)
        self.assertTrue(all((output / name).is_file() for name in FINANCIAL_OUTPUT))
        self.assertAlmostEqual(float(rows(output / "cashflows.csv")[0]["amount"]), 100000 * 0.10 * 30 / 365)

    def test_manifest_round_trips_native_path(self):
        input_path = self.folder / "deals.csv"
        output = self.folder / "out"
        self.write_deals(input_path, deal(1))

        self.run_cfcore(input_path, output)

        self.assertEqual(self.manifest(output)["input"], str(input_path))

    @unittest.skipIf(sys.platform == "win32", "Quotes and control characters are invalid in Windows filenames")
    def test_manifest_escapes_quotes_backslashes_and_controls(self):
        input_path = self.folder / 'deals"\\\t.csv'
        self.write_deals(input_path, deal(1))
        output = self.folder / "out"

        self.run_cfcore(input_path, output)

        self.assertEqual(self.manifest(output)["input"], str(input_path))

    def test_repricing_reports_keep_financial_exports_identical(self):
        input_path = self.folder / "deals.csv"
        fixings = self.folder / "fixings.csv"
        fixings.write_text("date,value\n2022-08-31,0.08\n", encoding="utf-8")
        self.write_deals(
            input_path,
            deal(1, rate_type_cd="FLOAT", interest_rate=0.02,
                 repricing_dttm_rate_schedule_fixed="2022-09-15,0.12"),
            deal(2, repricing_dttm_rate_schedule_fixed="2022-09-30,0.20",
                 principal_payment="2022-09-30,30000 2022-10-31,70000"),
            deal(3, rate_type_cd="FLOAT", interest_rate=0.02,
                 repricing_dttm_schedule="2022-09-15"),
            deal(4, rate_type_cd="FLOAT", interest_rate=0.02, base_indicator_rate=0.07,
                 repricing_dttm_schedule="2022-09-15"),
        )
        baseline = self.folder / "baseline"
        self.run_cfcore(input_path, baseline, "--fixings", str(fixings))
        self.assertFalse((baseline / "events.csv").exists())
        self.assertFalse((baseline / "deal_trace.csv").exists())

        traced = self.folder / "traced"
        self.run_cfcore(input_path, traced, "--fixings", str(fixings), "--events", "--trace-deal", "1")

        for name in FINANCIAL_OUTPUT:
            self.assertEqual((traced / name).read_bytes(), (baseline / name).read_bytes(), name)
        self.assertEqual(self.manifest(traced)["flows"], self.manifest(baseline)["flows"])

        events = rows(traced / "events.csv")
        self.assertEqual(len(events), 4)
        self.assertEqual([row["instrument_id"] for row in events], ["1", "2", "3", "4"])
        self.assertEqual([row["rate_source"] for row in events],
                         ["CONTRACT_SCHEDULE", "CONTRACT_SCHEDULE", "FIXING_HISTORY", "DEAL_INDICATOR"])
        self.assertEqual((events[0]["event_date"], float(events[0]["rate_before"]), float(events[0]["rate_after"])),
                         ("2022-09-15", 0.10, 0.20))
        for row in events:
            self.assertEqual(row["event_type"], "REPRICING")
            self.assertEqual(float(row["cash_amount"]), 0)

        trace = rows(traced / "deal_trace.csv")
        self.assertEqual({row["instrument_id"] for row in trace}, {"1"})
        first_coupon = trace[:2]
        self.assertEqual([row["event_type"] for row in first_coupon], ["REPRICING", "INTEREST"])
        self.assertEqual([row["period_start"] for row in first_coupon], ["2022-08-31", "2022-09-15"])
        self.assertEqual([row["period_end"] for row in first_coupon], ["2022-09-15", "2022-09-30"])
        self.assertEqual([int(row["days"]) for row in first_coupon], [15, 15])
        self.assertEqual([float(row["rate_before"]) for row in first_coupon], [0.10, 0.20])
        for row in first_coupon:
            self.assertAlmostEqual(float(row["year_fraction"]), 15 / 365, places=9)
            self.assertAlmostEqual(float(row["interest_accrued"]),
                                   100000 * float(row["rate_before"]) * 15 / 365, places=8)
        self.assertEqual(float(first_coupon[0]["interest_paid"]), 0)
        self.assertAlmostEqual(float(first_coupon[1]["interest_paid"]),
                               sum(float(row["interest_accrued"]) for row in first_coupon), places=8)

        same_day = self.folder / "same-day"
        self.run_cfcore(input_path, same_day, "--fixings", str(fixings),
                        "--trace-deal", "2", "--no-flows")
        self.assertFalse((same_day / "cashflows.csv").exists())
        self.assertFalse((same_day / "events.csv").exists())
        trace = rows(same_day / "deal_trace.csv")
        self.assertEqual({row["instrument_id"] for row in trace}, {"2"})
        sep30 = [row for row in trace if row["event_date"] == "2022-09-30"]
        self.assertEqual([row["event_type"] for row in sep30], ["INTEREST", "PRINCIPAL", "REPRICING"])
        self.assertEqual([int(row["days"]) for row in sep30], [30, 0, 0])
        self.assertEqual([float(row["balance_before"]) for row in sep30], [100000, 100000, 70000])
        self.assertEqual([float(row["balance_after"]) for row in sep30], [100000, 70000, 70000])
        self.assertEqual([float(row["principal_paid"]) for row in sep30], [0, 30000, 0])
        self.assertAlmostEqual(float(sep30[0]["interest_paid"]), 100000 * 0.10 * 30 / 365)
        self.assertEqual(float(sep30[-1]["cash_amount"]), 0)
        self.assertEqual(float(sep30[-1]["rate_after"]), 0.20)
        self.assertAlmostEqual(float(trace[len(sep30)]["interest_paid"]), 70000 * 0.20 * 31 / 365)

    def test_trace_records_implicit_final_interest(self):
        input_path = self.folder / "deals.csv"
        self.write_deals(input_path, deal(1, interest_payment="2022-09-30"))
        output = self.folder / "out"

        self.run_cfcore(input_path, output, "--trace-deal", "1")

        trace = rows(output / "deal_trace.csv")
        final_rows = [row for row in trace if row["event_date"] == "2022-10-31"]
        self.assertEqual([row["event_type"] for row in final_rows], ["PRINCIPAL", "INTEREST"])
        self.assertEqual([int(row["days"]) for row in final_rows], [31, 0])
        self.assertAlmostEqual(float(final_rows[0]["interest_accrued"]), 100000 * 0.10 * 31 / 365)
        self.assertAlmostEqual(float(final_rows[1]["interest_paid"]), float(final_rows[0]["interest_accrued"]))
        self.assertEqual(float(final_rows[1]["balance_after"]), 0)


if __name__ == "__main__":
    BINARY = Path(sys.argv.pop(1)).resolve()
    unittest.main()
