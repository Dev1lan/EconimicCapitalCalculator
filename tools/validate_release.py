"""Run the three policies on the MAI sample and audit output invariants."""

import argparse
from collections import Counter, defaultdict
import csv
from decimal import Decimal
import json
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
POLICIES = ("base", "legacy-excel", "contractual-v2")
MONEY_FILES = ("cashflows.csv", "deal_summary.csv", "buckets.csv", "errors.csv")
POLICY_LABELS = ("BASE", "LEGACY_EXCEL", "CONTRACTUAL_V2")
KINDS = ("PRINCIPAL", "INTEREST")


def rows(path, delimiter=","):
    with path.open(encoding="utf-8-sig", newline="") as file:
        return list(csv.DictReader(file, delimiter=delimiter))


def check(condition, message):
    if not condition:
        raise ValueError(message)


def run_policy(options, policy):
    out = options.output_dir / policy
    subprocess.run(
        [sys.executable, "-m", "normalizer", str(options.input), "--output-dir", str(out),
         "--format", "csv", "--cfcore", str(options.cfcore), "--fixings", str(options.fixings),
         "--threads", "1", "--policy", policy, "--events", "--trace-deal", "46"],
        check=True,
    )
    return out


def inspect(source, out, policy, reference):
    core = out / "core"
    normalized = rows(out / "normalized_deals.csv", ";")
    deals = {row["deal_id"]: row for row in normalized}
    quarantine = {row["deal_id"]: row["reason_code"] for row in rows(out / "quarantine.csv")}
    summary = {row["instrument_id"]: row for row in rows(core / "deal_summary.csv")}
    errors = {row["instrument_id"]: row["error_code"] for row in rows(core / "errors.csv")}
    groups = (set(summary), set(errors), set(quarantine))
    check(set(source) == set.union(*groups) and len(source) == sum(map(len, groups)), f"{policy}: partition")
    check(deals.keys() == summary.keys() | errors.keys(), f"{policy}: normalized rows")
    manifest = json.loads((core / "run_manifest.json").read_text(encoding="utf-8"))
    check(manifest["calculation_policy"] == policy, f"{policy}: manifest policy")

    amounts = defaultdict(Decimal)
    grouped_flows = defaultdict(Decimal)
    counts = Counter()
    for flow in rows(core / "cashflows.csv"):
        instrument = flow["instrument_id"]
        kind = flow["cash_flow_type"]
        amount = Decimal(flow["amount"])
        check(instrument in summary and kind in KINDS and amount >= 0, f"{policy}: invalid flow {flow}")
        counts[instrument] += 1
        amounts[instrument, kind] += amount
        deal = deals[instrument]
        grouped_flows[deal["portfolio_key"], deal["currency_id"], deal["side"], kind] += amount

    for identifier, row in summary.items():
        check(counts[identifier] == int(row["flow_count"]), f"{policy}: flow count for {identifier}")
        for kind, field in zip(KINDS, ("total_principal", "total_interest")):
            diff = Decimal(row[field]) - amounts[identifier, kind]
            check(abs(diff) < Decimal("0.0001"), f"{policy}: {field} for {identifier}")
        residual = Decimal(row["opening_principal"]) - amounts[identifier, "PRINCIPAL"]
        check(abs(residual - Decimal(row["closing_balance"])) < Decimal("0.0001"),
              f"{policy}: closing balance for {identifier}")
        if policy != "LEGACY_EXCEL":
            check(abs(residual) < Decimal("0.0001"), f"{policy}: unpaid principal for {identifier}")

    grouped_buckets = defaultdict(Decimal)
    for row in rows(core / "buckets.csv"):
        for kind, field in zip(KINDS, ("principal_cf", "interest_cf")):
            grouped_buckets[row["portfolio_key"], row["currency"], row["side"], kind] += Decimal(row[field])
    for key in grouped_buckets.keys() | grouped_flows.keys():
        check(abs(grouped_buckets[key] - grouped_flows[key]) < Decimal("0.01"), f"{policy}: bucket {key}")

    for event in rows(core / "events.csv"):
        check(event["instrument_id"] in summary and event["event_type"] == "REPRICING" and
              Decimal(event["cash_amount"]) == 0, f"{policy}: invalid repricing {event}")
    trace = rows(core / "deal_trace.csv")
    check(bool(trace) == ("46" in summary), f"{policy}: trace availability")
    for event in trace:
        check(event["instrument_id"] == "46", f"{policy}: trace ID")
    for kind, column in zip(KINDS, ("principal_paid", "interest_paid")):
        trace_sum = sum(Decimal(row[column]) for row in trace)
        check(abs(trace_sum - amounts["46", kind]) < Decimal("0.0001"), f"{policy}: trace and flow mismatch")

    counts_manifest = (manifest["deals_ok"], manifest["deals_failed"], manifest["flows"])
    check(counts_manifest == (len(summary), len(errors), sum(counts.values())), f"{policy}: manifest totals")
    if reference:
        for name in MONEY_FILES:
            check((core / name).read_bytes() == (reference / "core" / name).read_bytes(),
                  f"{policy}: iteration 4 differs in {name}")

    status = dict.fromkeys(summary, ("CALCULATED", ""))
    status.update({key: ("NORMALIZER_REJECTED", reason) for key, reason in quarantine.items()})
    status.update({key: ("CORE_REJECTED", reason) for key, reason in errors.items()})
    print(policy, "calculated", len(summary), "quarantine", len(quarantine),
          "core errors", len(errors), "payments", manifest["flows"])
    return status


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cfcore", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--input", type=Path, default=ROOT / "data/input/test_deals_mai.csv")
    parser.add_argument("--fixings", type=Path, default=ROOT / "data/input/key_rate.csv")
    parser.add_argument("--reference-dir", type=Path, help="optional iteration 4 mai-base, mai-excel and mai-v2 outputs")
    options = parser.parse_args()
    source_rows = rows(options.input, ";")
    source = {row["deal_id"]: row for row in source_rows}
    check(len(source_rows) == 55 and set(source) == {str(i) for i in range(1, 56)}, "expected original MAI IDs 1–55")
    statuses = []
    for policy, label, earlier in zip(POLICIES, POLICY_LABELS, ("mai-base", "mai-excel", "mai-v2")):
        result = run_policy(options, policy)
        reference = options.reference_dir / earlier if options.reference_dir else None
        statuses.append(inspect(source, result, label, reference))

    with (options.output_dir / "mai-deal-status.csv").open("w", encoding="utf-8", newline="") as output:
        writer = csv.writer(output, lineterminator="\n")
        writer.writerow(["deal_id", "currency", "amortization", "rate_type",
                         *(f"{policy}_{field}" for policy in ("base", "excel", "v2") for field in ("status", "reason"))])
        for identifier in sorted(source, key=int):
            raw = source[identifier]
            writer.writerow([identifier, raw["currency_id"], raw["discriminator"], raw["rate_type_cd"],
                             *(value for status in statuses for value in status[identifier])])


if __name__ == "__main__":
    main()
