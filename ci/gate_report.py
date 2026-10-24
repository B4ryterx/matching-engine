"""Posts the verification gates as a comment on the merge request (governance: every
merge has an auditable record of what was checked and the numbers it passed with)."""
import json, os, urllib.request
from pathlib import Path

rep = json.loads(Path("perf_report.json").read_text())
c = rep["checks"]
ok = "✅" if rep["passed"] else "❌"
body = (f"### {ok} Autopilot gates for `{os.environ.get('CI_COMMIT_SHORT_SHA', '')}`\n\n"
        f"| Gate | Result |\n|---|---|\n"
        f"| Byte-for-byte vs independent reference (3 × 200,000 random orders) | {'✅ identical' if rep['identical'] else '❌ MISMATCH'} |\n"
        f"| Mean latency | {c['mean_ns']['value']:.0f} ns (limit {c['mean_ns']['limit']:.0f}) {'✅' if c['mean_ns']['ok'] else '❌'} |\n"
        f"| p99 latency | {c['p99_ns']['value']} ns (limit {c['p99_ns']['limit']:.0f}) {'✅' if c['p99_ns']['ok'] else '❌'} |\n\n"
        f"Pipeline: {os.environ.get('CI_PIPELINE_URL', '')}")
url = f"{os.environ['CI_API_V4_URL']}/projects/{os.environ['CI_PROJECT_ID']}/merge_requests/{os.environ['CI_MERGE_REQUEST_IID']}/notes"
req = urllib.request.Request(url, data=json.dumps({"body": body}).encode(),
                             headers={"Content-Type": "application/json", "PRIVATE-TOKEN": os.environ["GITLAB_API_TOKEN"]})
urllib.request.urlopen(req).read()
print(body)
