"""Speed gate for CI, based on a same-machine A/B run (see ci/ab_bench.py).

Fails if the candidate is not byte-identical to the reference, or if it is slower than the base
version by more than `max_slowdown` (ratio of median mean latencies on the same runner).
Writes perf_report.json (pipeline artifact) and exits 1 on failure.
"""
import json, sys
from pathlib import Path

cfg = json.loads((Path(__file__).parent / "perf_baseline.json").read_text())
res = json.loads(Path(sys.argv[1]).read_text())
max_slow = cfg.get("max_slowdown", 0.15)
ratio_ok = res["ratio"] <= 1 + max_slow
report = {
    "identical": res["identical"],
    "candidate_mean_ns": res["candidate_mean_ns"], "base_mean_ns": res["base_mean_ns"],
    "ratio": res["ratio"], "max_ratio": 1 + max_slow, "ratio_ok": ratio_ok,
    "candidate_p99_ns": res["candidate_p99_ns"], "base_p99_ns": res["base_p99_ns"], "base_ref": res["base_ref"],
    "passed": res["identical"] and ratio_ok,
}
Path("perf_report.json").write_text(json.dumps(report, indent=2))
print(json.dumps(report, indent=2))
sys.exit(0 if report["passed"] else 1)
