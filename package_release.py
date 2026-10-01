#!/usr/bin/env python3
"""Package exactly the source-listed release files (and required event audits)."""

import argparse
import re
import zipfile
import tempfile
import concurrent.futures
from pathlib import Path
from audit_compression import compress_stream

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("directory", type=Path)
p.add_argument("source_urls", type=Path)
p.add_argument("archive", type=Path)
p.add_argument(
    "--jobs",
    type=int,
    default=3,
    help="Concurrent audit compressors (1-4; about 0.7 GiB RAM each)",
)
a = p.parse_args()
if not 1 <= a.jobs <= 4:
    raise SystemExit("--jobs must be between 1 and 4")
files = ["site_metadata.csv"]
for line in a.source_urls.read_text(encoding="utf-8").splitlines():
    line = line.split("#")[0].strip()
    if not line:
        continue
    m = re.fullmatch(
        r"https://dumps.wikimedia.org/([a-z0-9_]+wiki)/(\d{8})/\1-\2-pages-logging.xml.gz",
        line,
    )
    if not m:
        raise SystemExit("Invalid source URL: " + line)
    site, date = m.groups()
    files.extend(
        f"{date}/{site}.thanks.{kind}.csv"
        for kind in ("edges", "nodes", "target_resolution_audit")
    )
if len(files) != len(set(files)):
    raise SystemExit("Duplicate source entries")
for name in files:
    if not (a.directory / name).is_file():
        raise SystemExit("Missing release file: " + name)
if a.archive.exists():
    raise SystemExit("Refusing to overwrite existing archive")
with (
    tempfile.TemporaryDirectory(prefix="thanks-xz-") as tmp,
    concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool,
):

    def compress(name):
        dest = Path(tmp) / (Path(name).name + ".xz")
        with (a.directory / name).open("rb") as source:
            compress_stream(source, dest)
        print("Compressed " + name, flush=True)
        return dest

    audits = sorted(
        (n for n in files if n.endswith(".target_resolution_audit.csv")),
        key=lambda n: -(a.directory / n).stat().st_size,
    )
    futures = {name: pool.submit(compress, name) for name in audits}
    with zipfile.ZipFile(a.archive, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for name in sorted(files):
            if name in futures:
                z.write(
                    futures[name].result(),
                    name + ".xz",
                    compress_type=zipfile.ZIP_STORED,
                )
            else:
                z.write(a.directory / name, name)
print(a.archive)
