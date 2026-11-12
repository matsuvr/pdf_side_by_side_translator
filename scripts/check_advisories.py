"""Developer-only checks for the direct dependencies in third_party.lock.json."""
import json
from urllib.parse import urlencode
from urllib.request import Request, urlopen

from common import lock


def osv_ids(revision):
    query = {"commit": revision}
    while True:
        request = Request("https://api.osv.dev/v1/query", json.dumps(query).encode(),
                          {"Content-Type": "application/json"})
        with urlopen(request, timeout=45) as response:
            result = json.load(response)
        yield from (v["id"] for v in result.get("vulns", []) if not v.get("withdrawn"))
        if not result.get("next_page_token"):
            return
        query["page_token"] = result["next_page_token"]


def sqlite_ids(version):
    params = urlencode({"cpeName": f"cpe:2.3:a:sqlite:sqlite:{version}:*:*:*:*:*:*:*"})
    start = 0
    while True:
        url = f"https://services.nvd.nist.gov/rest/json/cves/2.0?{params}&isVulnerable&startIndex={start}"
        with urlopen(url, timeout=45) as response:
            result = json.load(response)
        entries = result["vulnerabilities"]
        yield from (item["cve"]["id"] for item in entries)
        start += len(entries)
        if start >= result["totalResults"]:
            return
        if not entries:
            raise RuntimeError("Incomplete NVD advisory response")


if __name__ == "__main__":
    findings = 0
    for name, entry in lock()["dependencies"].items():
        ids = sqlite_ids(entry["version"]) if name == "SQLite" else osv_ids(entry["revision"])
        matches = sorted(set(ids))
        if name == "SQLite" and entry["version"] == "3.54.1" and "CVE-2026-119586" in matches:
            print("SQLite CVE-2026-119586: CLI .recover only. Product links sqlite3.c and excludes the shell.")
            matches.remove("CVE-2026-119586")
        print(name, " ".join(matches) if matches else "no matches", flush=True)
        findings += len(matches)
    raise SystemExit(1 if findings else 0)
