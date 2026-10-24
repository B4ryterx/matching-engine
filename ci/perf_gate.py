"""Speed gate for CI: fail the pipeline if the engine got slower than the recorded baseline allows.

Usage: perf_gate.py <verify_result.json>
Reads ci/perf_baseline.json: {"mean_ns": ..., "p99_ns": ..., "tolerance": 0.25}
Writes perf_report.json (kept as a pipeline artifact) and exits 1 on a regression.
CI machines are noisy, so the tolerance is generous and the gate checks the median of several runs upstream.
"""
import json, sys
from pathlib import Path

base = json.loads((Path(__file__).parent / "perf_baseline.json").read_text())
res = json.loads(Path(sys.argv[1]).read_text())
tol = base.get("tolerance", 0.25)

checks = {}
for key in ("mean_ns", "p99_ns"):
    limit = base[key] * (1 + tol)
    checks[key] = {"value": res[key], "baseline": base[key], "limit": round(limit, 1), "ok": res[key] is not None and res[key] <= limit}

report = {"identical": res["identical"], "checks": checks, "passed": res["identical"] and all(c["ok"] for c in checks.values())}
Path("perf_report.json").write_text(json.dumps(report, indent=2))
print(json.dumps(report, indent=2))
sys.exit(0 if report["passed"] else 1)
