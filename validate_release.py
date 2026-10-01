#!/usr/bin/env python3
"""Check a publication archive against its logging source list (Python 3.8+)."""

import argparse

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("archive", help="Publication wikipedia-thanks-20260601.zip")
parser.add_argument("source_urls", help="Logging source_urls.txt")
parser.add_argument(
    "--report", default="release_validation.json", help="Output JSON report"
)
args = parser.parse_args()
from audit_compression import open_member
from audit_schema import AUDIT_FIELDS, NODE_FIELDS, EVENT_FIELDS, records
import csv
import io
import json
import re
import zipfile
import collections
import datetime
import hashlib
import ipaddress
import itertools
from pathlib import Path

archive = Path(args.archive)
urls = Path(args.source_urls)
pattern = re.compile(
    r"https://dumps\.wikimedia\.org/([a-z0-9_]+)wiki/(\d{8})/\1wiki-\2-pages-logging\.xml\.gz"
)
inputs = []
for line in urls.read_text(encoding="utf-8").splitlines():
    line = line.split("#", 1)[0].strip()
    if line:
        m = pattern.fullmatch(line)
        assert m, line
        inputs.append(m.groups())
assert len(set(inputs)) == len(inputs)
dates = {d for _, d in inputs}
assert len(dates) == 1, "Source URLs must specify one snapshot"
snapshot = dates.pop()
cutoff = datetime.datetime.strptime(snapshot, "%Y%m%d").strftime("%Y-%m-%dT00:00:00Z")
archive_hash = hashlib.sha256()
with archive.open("rb") as stream:
    for block in iter(lambda: stream.read(1024 * 1024), b""):
        archive_hash.update(block)
archive_sha256 = archive_hash.hexdigest()
result = {
    "archive_sha256": archive_sha256,
    "source_sites": len(inputs),
    "totals": {},
    "sites": [],
    "failures": [],
    "codebook": {},
    "headers": {},
}
EXPECTED_HEADERS = {
    "edges": ["site", "logid", "timestamp", "source", "target"],
    "nodes": [
        "site",
        "id",
        "node_type",
        "label",
        "creation_timestamp",
        "creation_evidence",
        "first_block_by_user_timestamp",
        "sent",
        "received",
    ],
    "node_metadata": [
        "site",
        "id",
        "node_type",
        "label",
        "creation_timestamp_missing",
        "creation_evidence",
        "creation_evidence_label",
        "sent",
        "received",
        "sent_to_resolved_target",
        "sent_to_unresolved_or_ambiguous_target",
        "received_interval_exact",
        "received_interval_and_current_username_same",
        "received_current_username_no_interval_conflict",
        "received_current_username_no_creation_timestamp",
        "received_other_resolved",
        "received_synthetic_missing_no_identity_evidence",
        "received_synthetic_ambiguous_multiple_active_intervals",
        "received_synthetic_missing_current_created_after_thank",
        "received_synthetic_missing_interval_account_created_after_thank",
        "received_synthetic_other_unresolved",
    ],
}
T = collections.Counter()
allcats = collections.Counter()
evidence = collections.defaultdict(set)


def check(ok, reason):
    if not ok:
        T["failed_checks"] += 1
        if len(result["failures"]) < 30:
            result["failures"].append(reason)


with zipfile.ZipFile(archive) as z:
    files = [n for n in z.namelist() if not n.endswith("/")]
    expected = {
        f"{snapshot}/{s}wiki.thanks.{kind}.csv"
        for s, _ in inputs
        for kind in ("edges", "nodes")
    } | {f"{snapshot}/{s}wiki.thanks.target_resolution_audit.csv.xz" for s, _ in inputs}
    expected.add("site_metadata.csv")
    check(set(files) == expected, "archive file set differs from source sites")
    check(len(files) == len(set(files)), "duplicate archive entries")
    context = list(
        csv.DictReader(io.TextIOWrapper(z.open("site_metadata.csv"), encoding="utf-8"))
    )
    check(
        {r["dump_network_site_code"] for r in context} == {s for s, _ in inputs},
        "site context coverage",
    )
    result["site_context_rows"] = len(context)
    result["site_code_aliases"] = [
        r for r in context if r["dump_network_site_code"] != r["actual_site_code"]
    ]
    for index, (site, _) in enumerate(inputs):
        C = collections.Counter()
        prefix = f"{snapshot}/{site}wiki.thanks."
        with z.open(prefix + "nodes.csv") as f:
            reader = csv.DictReader(io.TextIOWrapper(f, encoding="utf-8"))
            result["headers"]["nodes"] = reader.fieldnames
            check(reader.fieldnames == EXPECTED_HEADERS["nodes"], f"{site} node header")
            nodes = {}
            for r in reader:
                nid = int(r["id"])
                check(nid not in nodes, f"{site} duplicate node")
                nodes[nid] = r
                check(r["site"] == site, f"{site} node site")
                check(
                    (nid > 0 and r["node_type"] == "account")
                    or (nid < 0 and r["node_type"] == "synthetic_target_label"),
                    f"{site} node type",
                )
                check(int(r["sent"]) + int(r["received"]) > 0, f"{site} isolated node")
                for key in ("creation_timestamp", "first_block_by_user_timestamp"):
                    if r[key]:
                        try:
                            datetime.datetime.strptime(r[key], "%Y-%m-%dT%H:%M:%SZ")
                            check(r[key] < cutoff, f"{site} {key} cutoff")
                        except ValueError:
                            check(False, f"{site} invalid {key}")
                check(
                    nid > 0
                    or not r["creation_timestamp"]
                    and not r["first_block_by_user_timestamp"]
                    and int(r["sent"]) == 0,
                    f"{site} synthetic attributes",
                )
                C["nodes"] += 1
                C["account_nodes" if nid > 0 else "target_username_nodes"] += 1
                C["missing_creation_account_nodes"] += int(
                    nid > 0 and not r["creation_timestamp"]
                )
        sent = collections.Counter()
        received = collections.Counter()
        sresolved = collections.Counter()
        sunresolved = collections.Counter()
        logs = set()
        previous = None
        earliest = "9999"
        latest = ""
        with z.open(prefix + "edges.csv") as f:
            reader = csv.reader(io.TextIOWrapper(f, encoding="utf-8"))
            result["headers"]["edges"] = next(reader)
            check(
                result["headers"]["edges"] == EXPECTED_HEADERS["edges"],
                f"{site} edge header",
            )
            for s, l, t, a, b in reader:
                log = int(l)
                a = int(a)
                b = int(b)
                check(s == site, f"{site} edge site")
                check(
                    log > 0 and re.fullmatch(r"[1-9][0-9]*", l),
                    f"{site} positive logid",
                )
                check(log not in logs, f"{site} duplicate logid")
                logs.add(log)
                check(a > 0 and b != 0 and a != b, f"{site} edge identifiers/self-loop")
                check(a in nodes and b in nodes, f"{site} edge/node coverage")
                check(previous is None or previous <= (t, log), f"{site} edge sorting")
                previous = (t, log)
                check(
                    t < cutoff
                    and re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", t),
                    f"{site} timestamp",
                )
                try:
                    datetime.datetime.strptime(t, "%Y-%m-%dT%H:%M:%SZ")
                except ValueError:
                    check(False, f"{site} invalid Gregorian date")
                for nid in (a, b):
                    if nid > 0 and nodes[nid]["creation_timestamp"]:
                        check(
                            t >= nodes[nid]["creation_timestamp"],
                            f"{site} event before creation",
                        )
                sent[a] += 1
                received[b] += 1
                (sresolved if b > 0 else sunresolved)[a] += 1
                C["events"] += 1
                C["resolved_events" if b > 0 else "unresolved_events"] += 1
                earliest = min(earliest, t)
                latest = max(latest, t)
        audit_counts = collections.defaultdict(collections.Counter)
        with z.open(prefix + "edges.csv") as ef, open_member(
            z, prefix + "target_resolution_audit.csv"
        ) as af:
            er = csv.DictReader(io.TextIOWrapper(ef, encoding="utf-8"))
            ar = csv.DictReader(io.TextIOWrapper(af, encoding="utf-8"))
            required = [
                "site",
                "logid",
                "timestamp",
                "source",
                "target",
                "observed_target_label",
                "target_label_subtype",
                "target_resolution",
                "current_name_account_id",
                "current_name_creation_timestamp",
                "resolved_account_creation_timestamp",
                "interval_evidence",
                "requires_historical_review",
            ]
            check(ar.fieldnames == AUDIT_FIELDS, f"{site} audit header")
            for e, audit in itertools.zip_longest(er, records(ar, "event")):
                if e is None or audit is None:
                    check(False, f"{site} audit/edge row count")
                    continue
                check(
                    all(
                        e[k] == audit[k]
                        for k in ("site", "logid", "timestamp", "source", "target")
                    ),
                    f"{site} audit/edge disagreement",
                )
                try:
                    ipaddress.ip_address(audit["observed_target_label"])
                    subtype = "ip_address"
                except ValueError:
                    subtype = "username"
                check(
                    subtype == audit["target_label_subtype"], f"{site} target subtype"
                )
                target = int(audit["target"])
                if target < 0:
                    check(
                        nodes[target]["label"] == audit["observed_target_label"],
                        f"{site} synthetic label",
                    )
                reason = audit["target_resolution"]
                check(
                    audit["requires_historical_review"]
                    == str(int(reason == "missing_historical_label_confirmation")),
                    f"{site} historical review flag",
                )
                cat = "received_" + ("synthetic_" if target < 0 else "") + reason
                if cat not in EXPECTED_HEADERS["node_metadata"]:
                    cat = (
                        "received_synthetic_other_unresolved"
                        if target < 0
                        else "received_other_resolved"
                    )
                audit_counts[target][cat] += 1
                C["ip_target_events"] += int(subtype == "ip_address")
        seen = set()
        with open_member(z, prefix + "target_resolution_audit.csv") as f:
            reader = csv.DictReader(io.TextIOWrapper(f, encoding="utf-8"))
            result["headers"]["audit"] = reader.fieldnames
            check(reader.fieldnames == AUDIT_FIELDS, f"{site} unified audit header")
            cats = [k for k in NODE_FIELDS if k.startswith("received_")]
            for r in records(reader, "node"):
                nid = int(r["id"])
                check(nid in nodes and nid not in seen, f"{site} metadata ID")
                seen.add(nid)
                n = nodes[nid]
                for field in [
                    "site",
                    "node_type",
                    "label",
                    "creation_evidence",
                    "sent",
                    "received",
                ]:
                    check(n[field] == r[field], f"{site} metadata disagreement {field}")
                check(
                    int(r["creation_timestamp_missing"])
                    == int(not n["creation_timestamp"]),
                    f"{site} creation missing flag",
                )
                check(
                    int(n["sent"]) == sent[nid] and int(n["received"]) == received[nid],
                    f"{site} degrees",
                )
                check(
                    int(r["sent_to_resolved_target"]) == sresolved[nid]
                    and int(r["sent_to_unresolved_or_ambiguous_target"])
                    == sunresolved[nid],
                    f"{site} outgoing resolution accounting",
                )
                check(
                    sum(int(r[k]) for k in cats) == received[nid],
                    f"{site} incoming resolution accounting",
                )
                check(
                    sum(int(r[k]) for k in cats if ("synthetic" in k) == (nid > 0))
                    == 0,
                    f"{site} resolution/node type",
                )
                evidence[r["creation_evidence"]].add(r["creation_evidence_label"])
                for k in cats:
                    check(
                        int(r[k]) == audit_counts[nid][k],
                        f"{site} event audit category {k}",
                    )
                    allcats[k] += int(r[k])
        check(seen == set(nodes), f"{site} metadata coverage")
        C["sites"] = 1
        T.update(C)
        result["sites"].append(
            {"site": site, **C, "earliest": earliest, "latest": latest}
        )
        if (index + 1) % 50 == 0:
            print(
                f'Checked {index+1}/{len(inputs)} sites; {T["events"]:,} events',
                flush=True,
            )
result["totals"] = dict(T)
result["resolution_counts"] = dict(allcats)
result["codebook"] = {k: sorted(v) for k, v in evidence.items()}
result["earliest"] = min(r["earliest"] for r in result["sites"])
result["latest"] = max(r["latest"] for r in result["sites"])
result["top20"] = sorted(result["sites"], key=lambda x: x["events"], reverse=True)[:20]
result["top20_percent"] = sum(r["events"] for r in result["top20"]) / T["events"] * 100
result["test_totals"] = dict(
    sum(
        (
            collections.Counter({k: v for k, v in r.items() if isinstance(v, int)})
            for r in result["sites"]
            if r["site"] in ["test", "test2"]
        ),
        collections.Counter(),
    )
)
Path(args.report).write_text(json.dumps(result, indent=2), encoding="utf-8")
print(
    json.dumps(
        {k: v for k, v in result.items() if k not in ["sites", "headers", "top20"]},
        indent=2,
    ),
    flush=True,
)

raise SystemExit(1 if result["failures"] else 0)
