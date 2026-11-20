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


if __name__ == "__main__":
    BINARY = Path(sys.argv.pop(1)).resolve()
    unittest.main()
