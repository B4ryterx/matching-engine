"""One end-to-end verification run of the engine, used by CI, the live service and the probe.

1. Generate a random order flow and a reference answer from an independent Python exchange.
2. Run the C++ engine on the same flow.
3. Compare outputs byte for byte, then replay the reference and read the speed figures.

Prints and returns a JSON-able dict:
  {"identical": bool, "orders": n, "mean_ns": float, "p99_ns": int, "p999_ns": int, "seconds": float}
"""
import json, re, subprocess, sys, tempfile, time, filecmp
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "build"


def run(n_orders=200_000, seed=1, levels=10, bin_dir=None):
    bin_dir = Path(bin_dir) if bin_dir else BIN
    t0 = time.time()
    with tempfile.TemporaryDirectory() as d:
        subprocess.run([sys.executable, "-I", str(ROOT / "tools" / "gen_flow.py"), d, str(n_orders), str(levels), str(seed)],
                       check=True, capture_output=True)
        subprocess.run([str(bin_dir / "match_flow"), f"{d}/flow.csv", f"{d}/out_message.csv", f"{d}/out_orderbook.csv", str(levels)],
                       check=True, capture_output=True)
        identical = (filecmp.cmp(f"{d}/out_message.csv", f"{d}/ref_message.csv", shallow=False)
                     and filecmp.cmp(f"{d}/out_orderbook.csv", f"{d}/ref_orderbook.csv", shallow=False))
        rep = subprocess.run([str(bin_dir / "replay_lobster"), f"{d}/ref_message.csv", f"{d}/ref_orderbook.csv"],
                             capture_output=True, text=True)
    out = rep.stdout
    mean = re.search(r"mean (\d+) ns per message", out)
    pct = re.search(r"99% (\d+) ns \| 99\.9% (\d+) ns", out)
    result = {
        "identical": bool(identical) and rep.returncode == 0,
        "orders": n_orders,
        "seed": seed,
        "mean_ns": float(mean.group(1)) if mean else None,
        "p99_ns": int(pct.group(1)) if pct else None,
        "p999_ns": int(pct.group(2)) if pct else None,
        "seconds": round(time.time() - t0, 2),
    }
    return result


if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 200_000
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    r = run(n, seed)
    print(json.dumps(r))
    sys.exit(0 if r["identical"] else 1)
