#!/usr/bin/env python3
"""PSBET Phase-5 performance gate.

Compares `psbet bench --json` output against ci/perf_budgets.json floors.

Policy (documented in PHASE_5_REPORT §7):
  * CI (shared runners): enforce conservative FPS floors that catch real
    regressions (>>10%) while tolerating runner variance. Floors are
    hardware-derived from the Phase-5 measurement campaign (2-vCPU sandbox
    baseline / 2.6, rounded down).
  * The roadmap's strict 2% rule applies to same-machine A/B comparisons:
    run `psbet bench --json before.json`, apply your change, run
    `psbet bench --json after.json` on the SAME machine and diff — no floor
    juggling needed. CI floors exist because shared runners cannot hold 2%.
  * The Vulkan case is reported but never gated (llvmpipe variance is too
    high to be a meaningful CI signal; real-GPU numbers are user-side).

Usage:
  python3 ci/perf_gate.py <bench.json>            # gate against budgets
  python3 ci/perf_gate.py --update <bench.json>   # rewrite budgets from bench
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BUDGETS = os.path.join(HERE, "perf_budgets.json")


def load(path):
    with open(path) as f:
        return json.load(f)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    update = "--update" in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        print(__doc__)
        return 2
    bench = load(args[0])
    cases = {c["name"]: c for c in bench.get("cases", [])}

    if update:
        budgets = {n: {"minFps": round(c["avgFps"] * 0.98, 2)}
                   for n, c in cases.items() if c.get("frames", 0) > 0}
        with open(BUDGETS, "w") as f:
            json.dump(budgets, f, indent=2)
            f.write("\n")
        print(f"perf_gate: budgets updated from {args[0]} -> {BUDGETS}")
        return 0

    if not os.path.exists(BUDGETS):
        print("perf_gate: FAIL — no budget file (commit ci/perf_budgets.json "
              "from a baseline run: `python3 ci/perf_gate.py --update bench.json`)")
        return 1
    budgets = load(BUDGETS)

    failed = 0
    for name, budget in sorted(budgets.items()):
        case = cases.get(name)
        if not case or case.get("frames", 0) == 0:
            print(f"perf_gate: {name:12s} MISSING from bench output — FAIL")
            failed += 1
            continue
        fps = case["avgFps"]
        floor = float(budget["minFps"])
        # 5% hysteresis below the floor: one warning, two floors = fail, so
        # single-run jitter does not flake the gate but a real regression does.
        status = "OK "
        if fps < floor * 0.95:
            status = "FAIL"
            failed += 1
        elif fps < floor:
            status = "WARN"
        print(f"perf_gate: {status} {name:12s} {fps:8.1f} fps "
              f"(floor {floor:8.1f})")

    # Informational: the Vulkan case.
    if "vk-720p" in cases:
        c = cases["vk-720p"]
        if c.get("frames", 0) > 0:
            print(f"perf_gate: INFO vk-720p       {c['avgFps']:8.1f} fps "
                  f"(advisory, not gated)")

    print(f"perf_gate: {'FAIL' if failed else 'PASS'} "
          f"({failed} case(s) below floor)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
