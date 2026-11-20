"""INP-4: необязательный регрессионный прогон настоящего C++-ядра.

CFCORE_BIN и CFCORE_FIXINGS указывают на бинарник и исторические ставки
из Dev1lan/EconimicCapitalCalculator (develop/core, commit 6fa3b5c).
Без них тест пропускается; обычные тесты не зависят от чужого репозитория.
"""

import csv
import hashlib
import json
import os
from pathlib import Path

import pytest

from normalizer.__main__ import main


ROOT = Path(__file__).resolve().parents[1]
SAMPLE = ROOT / "tests/data/тестовые_сделки_МАИ.csv"
GOLDEN = ROOT / "tests/golden"
FIXINGS_SHA256 = "8b02d6c5b037689c5563addfb55af116e0a26da8ad89281924875fff935a5c5a"


def _read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as file:
        return list(csv.DictReader(file))


def test_pinned_cfcore_on_all_55_deals(tmp_path):
    cfcore_path = os.environ.get("CFCORE_BIN")
    fixings_path = os.environ.get("CFCORE_FIXINGS")
    if not cfcore_path and not fixings_path:
        pytest.skip("Для проверки настоящего ядра задайте CFCORE_BIN и CFCORE_FIXINGS")
    if not cfcore_path or not fixings_path:
        pytest.fail("Нужно задать одновременно CFCORE_BIN и CFCORE_FIXINGS")

    cfcore = Path(cfcore_path).resolve()
    fixings = Path(fixings_path).resolve()
    assert cfcore.is_file(), f"Нет исполняемого C++-ядра: {cfcore}"
    assert fixings.is_file(), f"Нет файла исторических ставок: {fixings}"
    assert hashlib.sha256(fixings.read_bytes()).hexdigest() == FIXINGS_SHA256, (
        "История ставок отличается от зафиксированной в INP-4: "
        "нужен data/input/key_rate.csv из ядра коммита 6fa3b5c"
    )

    out = tmp_path / "result"
    assert main([
        str(SAMPLE), "--output-dir", str(out), "--format", "both",
        "--cfcore", str(cfcore), "--fixings", str(fixings), "--threads", "1",
    ]) == 0

    core = out / "core"
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
