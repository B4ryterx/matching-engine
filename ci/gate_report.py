"""Posts the verification gates as a comment on the merge request (governance: every
merge has an auditable record of what was checked and the numbers it passed with)."""
import json, os, urllib.request
from pathlib import Path

rep = json.loads(Path("perf_report.json").read_text())
ok = "✅" if rep["passed"] else "❌"
body = (f"### {ok} Autopilot gates for `{os.environ.get('CI_COMMIT_SHORT_SHA', '')}`\n\n"
        f"| Gate | Result |\n|---|---|\n"
        f"| Byte-for-byte vs independent reference (3 × 200,000 random orders) | {'✅ identical' if rep['identical'] else '❌ MISMATCH'} |\n"
        f"| Speed, A/B on the same runner vs `{rep['base_ref']}` | {rep['candidate_mean_ns']:.0f} ns vs {rep['base_mean_ns']:.0f} ns, ratio {rep['ratio']:.3f} (max {rep['max_ratio']:.2f}) {'✅' if rep['ratio_ok'] else '❌'} |\n"
        f"| p99 latency | {rep['candidate_p99_ns']} ns vs {rep['base_p99_ns']} ns | \n\n"
        f"Pipeline: {os.environ.get('CI_PIPELINE_URL', '')}")
url = f"{os.environ['CI_API_V4_URL']}/projects/{os.environ['CI_PROJECT_ID']}/merge_requests/{os.environ['CI_MERGE_REQUEST_IID']}/notes"
req = urllib.request.Request(url, data=json.dumps({"body": body}).encode(),
                             headers={"Content-Type": "application/json", "PRIVATE-TOKEN": os.environ["GITLAB_API_TOKEN"]})
urllib.request.urlopen(req).read()
print(body)
