"""Correctness + same-machine A/B speed check, used by the CI `byte-for-byte` job.

Shared CI runners differ in speed by 30% or more, so an absolute latency threshold produces false
alarms. Instead this builds the BASE version of the engine (the merge request's target branch, or the
previous commit on main) next to the CANDIDATE on the same machine, runs them alternately on the
same random order flows, and gates on the ratio candidate / base.

Writes verify.json:
  {"identical": bool, "candidate_mean_ns", "base_mean_ns", "ratio", "candidate_p99_ns", "base_p99_ns", "base_ref"}
"""
import json, os, statistics as st, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import verify_run

ROOT = Path(__file__).resolve().parent.parent
BASE_DIR = Path("/tmp/engine_base")


def sh(*cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, cwd=kw.get("cwd", ROOT)).stdout.strip()


def base_ref():
    target = os.environ.get("CI_MERGE_REQUEST_TARGET_BRANCH_NAME")
    if target:
        subprocess.run(["git", "fetch", "--depth", "50", "origin", target], cwd=ROOT, capture_output=True)
        return sh("git", "rev-parse", "FETCH_HEAD")
    subprocess.run(["git", "fetch", "--deepen", "1"], cwd=ROOT, capture_output=True)   # no-op on full clones
    return sh("git", "rev-parse", "HEAD~1")


def main(seeds=(1, 2, 3), n=200_000):
    ref = base_ref()
    if BASE_DIR.exists():
        sh("git", "worktree", "remove", "--force", str(BASE_DIR))
    sh("git", "worktree", "add", "--detach", str(BASE_DIR), ref)
    subprocess.run(["make", "-C", str(BASE_DIR), "all", "CXX=" + os.environ.get("CXX", "g++")], check=True,
                   capture_output=True)

    cand, base = [], []
    for s in seeds:                      # alternate so both see the same machine conditions
        cand.append(verify_run.run(n, s))
        base.append(verify_run.run(n, s, bin_dir=BASE_DIR / "build"))
        print(f"seed {s}: candidate {cand[-1]['mean_ns']} ns, base {base[-1]['mean_ns']} ns, identical={cand[-1]['identical']}")

    cm, bm = st.median(r["mean_ns"] for r in cand), st.median(r["mean_ns"] for r in base)
    res = {
        "identical": all(r["identical"] for r in cand),
        "candidate_mean_ns": cm, "base_mean_ns": bm, "ratio": round(cm / bm, 3),
        "candidate_p99_ns": st.median(r["p99_ns"] for r in cand), "base_p99_ns": st.median(r["p99_ns"] for r in base),
        "base_ref": ref[:8],
    }
    Path("verify.json").write_text(json.dumps(res, indent=2))
    print(json.dumps(res, indent=2))


if __name__ == "__main__":
    main()
