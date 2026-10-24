"""Production probe (runs on a pipeline schedule).

Calls the live service's /verify. If the engine's output is no longer identical to the reference,
or it has slowed beyond the baseline, opens a GitLab issue labelled `incident` with the evidence.
That issue is what wakes the triage flow up, closing the loop without a human.

Env: LIVE_URL, CI_API_V4_URL, CI_PROJECT_ID, PROBE_TOKEN (project access token, api scope)
"""
import json, os, sys, urllib.request
from pathlib import Path

base = json.loads((Path(__file__).parent / "perf_baseline.json").read_text())
url = os.environ["LIVE_URL"].rstrip("/") + "/verify"

try:
    with urllib.request.urlopen(url, timeout=60) as r:
        res = json.loads(r.read())
except Exception as e:
    res = {"identical": False, "error": f"probe could not reach {url}: {e}"}

problems = []
if not res.get("identical"):
    problems.append("output is NOT byte-identical to the reference exchange" + (f" ({res['error']})" if "error" in res else ""))
limit = base["mean_ns"] * (1 + base.get("tolerance", 0.25)) * base.get("prod_factor", 2.0)
if res.get("mean_ns") and res["mean_ns"] > limit:
    problems.append(f"mean latency {res['mean_ns']:.0f} ns exceeds the production limit of {limit:.0f} ns")

print(json.dumps(res))
if not problems:
    print("healthy")
    sys.exit(0)

body = "The scheduled production probe found a problem:\n\n" + "\n".join(f"- {p}" for p in problems)
body += f"\n\nProbe result:\n```json\n{json.dumps(res, indent=2)}\n```\nPipeline: {os.environ.get('CI_PIPELINE_URL', '')}"
req = urllib.request.Request(
    f"{os.environ['CI_API_V4_URL']}/projects/{os.environ['CI_PROJECT_ID']}/issues",
    data=json.dumps({"title": "Incident: production probe failed", "description": body, "labels": "incident,automated"}).encode(),
    headers={"Content-Type": "application/json", "PRIVATE-TOKEN": os.environ["PROBE_TOKEN"]},
)
with urllib.request.urlopen(req) as r:
    print("opened issue", json.loads(r.read())["web_url"])
sys.exit(1)
