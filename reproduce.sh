#!/usr/bin/env bash
# Reconstruct a release in a fresh directory; no previous release is required.
set -Eeuo pipefail
export PYTHONUTF8=1 PYTHONIOENCODING=utf-8
if (($# != 1)) || [[ $1 != /* ]]; then
  echo 'Usage: bash reproduce.sh /absolute/new-run-directory' >&2
  exit 2
fi
CODE=$(cd -- "$(dirname -- "$0")" && pwd)
RUN=$1
[[ ! -e "$RUN" ]] || {
  echo 'Run directory must not already exist.' >&2
  exit 2
}
mkdir -p "$RUN"/{build,gz,out,reports}
exec > >(tee "$RUN/run.log") 2>&1
trap 'echo "FAILED at line $LINENO; the requested reconstruction checks have not all passed." >&2' ERR
cp "$CODE"/{extract_thanks.cpp,extract_thanks_aux.cpp,compile.sh} "$RUN/build/"
(cd "$RUN/build" && bash compile.sh)
REQUIRE_MD5=1 bash "$CODE/download_source_dumps.sh" "$CODE/source_urls.txt" "$RUN/gz" 3
bash "$CODE/run_extract_thanks_all.sh" "$RUN/gz" "$RUN/out" "$RUN/build/extract_thanks"
REQUIRE_MD5=1 bash "$CODE/generate_site_metadata.sh" "$CODE/source_urls.txt" \
  --site-stats-dir "$RUN/site_stats" \
  --out-csv "$RUN/out/site_metadata.csv"
python3 "$CODE/package_release.py" "$RUN/out" "$CODE/source_urls.txt" "$RUN/wikipedia-thanks-20260601.zip"
python3 "$CODE/validate_release.py" "$RUN/wikipedia-thanks-20260601.zip" "$CODE/source_urls.txt" \
  --report "$RUN/reports/release_validation.json"
sha256sum "$RUN/wikipedia-thanks-20260601.zip" > "$RUN/reports/release.sha256"
find "$RUN/gz" -maxdepth 1 -name '*-pages-logging.xml.gz' -type f -print0 | sort -z | xargs -0 sha256sum > "$RUN/reports/logging_inputs.sha256"
find "$RUN/site_stats" -maxdepth 1 -name '*-site_stats.sql.gz' -type f -print0 | sort -z | xargs -0 sha256sum > "$RUN/reports/site_stats_inputs.sha256"
echo 'CSV reconstruction and mandatory validation passed.'
