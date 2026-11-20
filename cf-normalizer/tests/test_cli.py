import csv
import json
import os
from pathlib import Path
import subprocess
import sys

import pyarrow.parquet as pq
import pytest

from normalizer.__main__ import main


ROOT = Path(__file__).resolve().parents[1]
SAMPLE = ROOT / "tests/data/тестовые_сделки_МАИ.csv"


def test_cli_writes_all_three_files(tmp_path, capsys):
    out = tmp_path / "normalizer-out"
    assert main([str(SAMPLE), "--output-dir", str(out)]) == 0
    assert (out / "normalized_deals.csv").is_file()
    assert (out / "normalized_deals.parquet").is_file()
    assert (out / "quarantine.csv").is_file()
    assert pq.read_table(out / "normalized_deals.parquet").num_rows == 45
    assert "принято: 45; отклонено: 10" in capsys.readouterr().out


def test_cli_csv_without_fixings_keeps_indicator(tmp_path):
    out = tmp_path / "out"
    assert main([str(SAMPLE), "--output-dir", str(out), "--format", "csv"]) == 0
    assert not (out / "normalized_deals.parquet").exists()
    with (out / "normalized_deals.csv").open(encoding="utf-8", newline="") as file:
        rows = list(csv.DictReader(file, delimiter=";"))
    row42 = next(row for row in rows if row["deal_id"] == "42")
    assert row42["rate_type_cd"] == "FLOAT"
    assert row42["base_indicator_rate"] == "0.08"


def test_cli_parquet_only_does_not_create_core_csv(tmp_path):
    out = tmp_path / "out"
    assert main([str(SAMPLE), "--output-dir", str(out), "--format", "parquet"]) == 0
    assert (out / "normalized_deals.parquet").exists()
    assert (out / "quarantine.csv").exists()
    assert not (out / "normalized_deals.csv").exists()


def test_python_m_normalizer_entrypoint(tmp_path):
    env = {**os.environ, "PYTHONPATH": str(ROOT / "src")}
    completed = subprocess.run(
        [sys.executable, "-m", "normalizer", str(SAMPLE), "--output-dir", str(tmp_path),
         "--format", "csv"],
        env=env, check=False, text=True, capture_output=True,
    )
    assert completed.returncode == 0, completed.stderr
    assert (tmp_path / "normalized_deals.csv").exists()


@pytest.mark.skipif(sys.platform == "win32", reason="исполняемый скрипт POSIX")
def test_cli_passes_normalized_csv_to_cfcore(tmp_path):
    fake = tmp_path / "fake_cfcore"
    fake.write_text(
        f"#!{sys.executable}\n"
        "import argparse, json\n"
        "from pathlib import Path\n"
        "p = argparse.ArgumentParser()\n"
        "p.add_argument('--input')\n"
        "p.add_argument('--output-dir')\n"
        "p.add_argument('--fixings')\n"
        "p.add_argument('--threads')\n"
        "a = p.parse_args()\n"
        "out = Path(a.output_dir)\n"
        "out.mkdir(parents=True, exist_ok=True)\n"
        "(out / 'args.json').write_text(json.dumps(vars(a)), encoding='utf8')\n",
        encoding="utf-8",
    )
    fake.chmod(0o755)
    fixings = tmp_path / "key_rate.csv"
    fixings.write_text("date,value\n2022-08-31,0.08\n", encoding="utf-8")
    out = tmp_path / "result"

    assert main([
        str(SAMPLE), "--output-dir", str(out), "--format", "both",
        "--cfcore", str(fake), "--fixings", str(fixings), "--threads", "2",
    ]) == 0
    arguments = json.loads((out / "core/args.json").read_text(encoding="utf-8"))
    assert arguments["input"] == str((out / "normalized_deals.csv").resolve())
    assert arguments["fixings"] == str(fixings.resolve())
    assert arguments["threads"] == "2"
    with (out / "normalized_deals.csv").open(encoding="utf-8", newline="") as file:
        row42 = next(row for row in csv.DictReader(file, delimiter=";") if row["deal_id"] == "42")
    assert row42["base_indicator_rate"] == ""


def test_cli_rejects_parquet_only_for_cfcore(tmp_path):
    fake = tmp_path / "fake_cfcore"
    fake.write_text("", encoding="utf-8")
    with pytest.raises(SystemExit) as exc:
        main([
            str(SAMPLE), "--output-dir", str(tmp_path / "out"),
            "--format", "parquet", "--cfcore", str(fake),
        ])
    assert exc.value.code == 2


def test_cli_rejects_invalid_threads(tmp_path):
    with pytest.raises(SystemExit) as exc:
        main([str(SAMPLE), "--output-dir", str(tmp_path), "--threads", "0"])
    assert exc.value.code == 2
