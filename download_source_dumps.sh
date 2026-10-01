#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  cat >&2 << 'EOF'
Usage:
  download_source_dumps.sh SOURCE_URLS.txt OUT_DIR [JOBS]

Example:
  ./download_source_dumps.sh source_urls.txt gz 3

Environment variables:
  REQUIRE_MD5=1    fail if the Wikimedia md5sums file or entry is unavailable
  FAST_SKIP=1      skip existing .gz files without re-validating gzip/md5
  CURL_RATE=10M    optional curl speed limit per download, e.g. 10M
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if [[ $# -lt 2 || $# -gt 3 ]]; then
  usage
  exit 1
fi

URL_FILE=$1
OUT_DIR=$2
JOBS=${3:-3}

REQUIRE_MD5=${REQUIRE_MD5:-0}
FAST_SKIP=${FAST_SKIP:-0}
CURL_RATE=${CURL_RATE:-}

if [[ ! -f "$URL_FILE" ]]; then
  echo "ERROR: source URL file not found: $URL_FILE" >&2
  exit 1
fi

if ! [[ "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo "ERROR: JOBS must be a positive integer." >&2
  exit 1
fi

need_cmd() {
  command -v "$1" > /dev/null 2>&1 || {
    echo "ERROR: required command not found: $1" >&2
    exit 1
  }
}

need_cmd bash
need_cmd curl
need_cmd gzip
need_cmd md5sum
need_cmd awk
need_cmd sed
need_cmd grep
need_cmd xargs

mkdir -p "$OUT_DIR"
LOG_DIR="$OUT_DIR/.logs"
MD5_DIR="$OUT_DIR/.md5sums"
mkdir -p "$LOG_DIR" "$MD5_DIR"

normalize_urls() {
  sed 's/\r$//' "$URL_FILE" |
    sed 's/[[:space:]]*#.*$//' |
    sed 's/^[[:space:]]*//; s/[[:space:]]*$//' |
    awk 'NF > 0'
}

download_text_file() {
  local url=$1
  local out=$2
  local tmp="${out}.part"

  if [[ -s "$out" ]]; then
    return 0
  fi

  curl -fL --retry 10 --retry-all-errors --connect-timeout 30 \
    --speed-time 120 --speed-limit 1024 \
    --output "$tmp" "$url"

  mv -f "$tmp" "$out"
}

remote_md5_for_url() {
  local url=$1
  local filename prefix dir md5_url md5_file md5

  filename=${url##*/}
  prefix=${filename%-pages-logging.xml.gz}
  dir=${url%/*}
  md5_url="${dir}/${prefix}-md5sums.txt"
  md5_file="$MD5_DIR/${prefix}-md5sums.txt"

  if [[ ! -s "$md5_file" ]]; then
    if ! download_text_file "$md5_url" "$md5_file" > /dev/null 2>&1; then
      if [[ "$REQUIRE_MD5" == "1" ]]; then
        echo "ERROR: could not download md5sums file: $md5_url" >&2
        return 2
      fi
      return 1
    fi
  fi

  md5=$(awk -v f="$filename" '$2 == f {print $1; exit}' "$md5_file")
  if [[ -z "$md5" ]]; then
    if [[ "$REQUIRE_MD5" == "1" ]]; then
      echo "ERROR: md5 entry not found for $filename in $md5_file" >&2
      return 2
    fi
    return 1
  fi

  printf '%s\n' "$md5"
}

check_md5_if_available() {
  local url=$1
  local path=$2
  local expected actual

  if ! expected=$(remote_md5_for_url "$url"); then
    if [[ "$REQUIRE_MD5" == "1" ]]; then
      return 1
    fi
    return 0
  fi

  actual=$(md5sum "$path" | awk '{print $1}')
  if [[ "$actual" != "$expected" ]]; then
    echo "ERROR: MD5 mismatch for $path" >&2
    echo "       expected: $expected" >&2
    echo "       actual:   $actual" >&2
    return 1
  fi
}

download_one() {
  local url=$1
  local filename final part log curl_args actual_url

  actual_url=$url
  filename=${actual_url##*/}
  final="$OUT_DIR/$filename"
  part="$final.part"
  log="$LOG_DIR/$filename.log"

  {
    echo "[$(date -Is)] START $actual_url"

    if [[ ! "$actual_url" =~ ^https://dumps\.wikimedia\.org/.+\.xml\.gz$ ]]; then
      echo "ERROR: URL does not look like a Wikimedia .xml.gz dump URL: $actual_url" >&2
      exit 1
    fi

    if [[ ! "$filename" =~ ^[a-z0-9_]{2,20}wiki-20[0-9][0-9][01][0-9][0-3][0-9]-pages-logging\.xml\.gz$ ]]; then
      echo "ERROR: filename does not match expected logging dump pattern: $filename" >&2
      exit 1
    fi

    if [[ "$FAST_SKIP" == "1" && -s "$final" ]]; then
      echo "[$(date -Is)] FAST_SKIP existing file: $final"
      exit 0
    fi

    if [[ -s "$final" ]]; then
      echo "[$(date -Is)] Existing file found; validating: $final"
      if gzip -t "$final" && check_md5_if_available "$actual_url" "$final"; then
        echo "[$(date -Is)] OK existing file: $final"
        exit 0
      fi

      bad="${final}.bad.$(date +%Y%m%dT%H%M%S)"
      echo "[$(date -Is)] Existing file failed validation; moving to: $bad"
      mv -f "$final" "$bad"
    fi

    curl_args=(
      -fL
      --retry 20
      --retry-all-errors
      --connect-timeout 30
      --speed-time 120
      --speed-limit 1024
      -C -
      --output "$part"
    )

    if [[ -n "$CURL_RATE" ]]; then
      curl_args+=(--limit-rate "$CURL_RATE")
    fi

    echo "[$(date -Is)] Downloading/resuming to: $part"
    curl "${curl_args[@]}" "$actual_url"

    echo "[$(date -Is)] Validating gzip: $part"
    gzip -t "$part"

    echo "[$(date -Is)] Validating MD5 if available"
    check_md5_if_available "$actual_url" "$part"

    mv -f "$part" "$final"
    echo "[$(date -Is)] DONE $final"
  } > "$log" 2>&1
}

export OUT_DIR LOG_DIR MD5_DIR REQUIRE_MD5 FAST_SKIP CURL_RATE
export -f download_one download_text_file remote_md5_for_url check_md5_if_available 2> /dev/null || true

tmp_urls=$(mktemp)
trap 'rm -f "$tmp_urls"' EXIT

normalize_urls | sort -u > "$tmp_urls"

n_urls=$(wc -l < "$tmp_urls" | tr -d ' ')
if [[ "$n_urls" == "0" ]]; then
  echo "ERROR: no URLs found in $URL_FILE" >&2
  exit 1
fi

echo "URLs: $n_urls"
echo "Output directory: $OUT_DIR"
echo "Parallel downloads: $JOBS"
echo "Logs: $LOG_DIR"
echo

if xargs -a "$tmp_urls" -r -n 1 -P "$JOBS" bash -c 'download_one "$1"' _; then
  echo
  echo "All downloads completed successfully."
else
  echo
  echo "ERROR: at least one download failed. Inspect logs in: $LOG_DIR" >&2
  echo "Recent failing-looking logs:" >&2
  grep -RilE '(^ERROR:|^curl:|^gzip:|not in gzip format|unexpected end of file|The requested URL returned error|Failed writing body|Connection timed out|Operation timed out)' "$LOG_DIR" 2> /dev/null | tail -20 >&2 || true
  exit 1
fi
