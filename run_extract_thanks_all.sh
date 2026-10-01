#!/usr/bin/env bash
set -uo pipefail

# Sequentially process MediaWiki pages-logging dumps with extract_thanks.
# Skips a dump only when its three expected publication CSV outputs already
# exist, are non-empty, and have the expected headers.
#
# Usage:
#   ./run_extract_thanks_all.sh GZ_DIR OUT_DIR [EXTRACT_THANKS_BIN]
#
# Example:
#   ./run_extract_thanks_all.sh gz out ./extract_thanks

GZ_DIR="${1:-}"
OUT_DIR="${2:-}"
EXTRACT_BIN="${3:-./extract_thanks}"
BUFFER_MB=64

usage() {
  cat >&2 << 'EOF'
Usage:
  ./run_extract_thanks_all.sh GZ_DIR OUT_DIR [EXTRACT_THANKS_BIN]

Example:
  ./run_extract_thanks_all.sh gz out ./extract_thanks
EOF
}

if [[ -z "$GZ_DIR" || -z "$OUT_DIR" || $# -gt 3 ]]; then
  usage
  exit 2
fi

if [[ ! -d "$GZ_DIR" ]]; then
  echo "ERROR: GZ_DIR is not a directory: $GZ_DIR" >&2
  exit 2
fi

if [[ ! -x "$EXTRACT_BIN" ]]; then
  echo "ERROR: extract_thanks binary is not executable: $EXTRACT_BIN" >&2
  exit 2
fi

mkdir -p "$OUT_DIR"

GZ_DIR_ABS="$(cd "$GZ_DIR" && pwd)"
OUT_DIR_ABS="$(cd "$OUT_DIR" && pwd)"
if [[ "$EXTRACT_BIN" == */* ]]; then
  EXTRACT_BIN_ABS="$(cd "$(dirname "$EXTRACT_BIN")" && pwd)/$(basename "$EXTRACT_BIN")"
else
  EXTRACT_BIN_ABS="$EXTRACT_BIN"
fi

LOG_FILE="$OUT_DIR_ABS/extract_thanks_$(date +%Y%m%d_%H%M%S).log"

mapfile -d '' DUMPS < <(find "$GZ_DIR_ABS" -maxdepth 1 -type f -name '*-pages-logging.xml.gz' -print0 | sort -z)

EDGE_CSV=""
NODE_CSV=""
AUDIT_CSV=""

expected_output_paths_for_dump() {
  local dump filename site dump_date out_date_dir base
  dump="$1"
  filename="$(basename -- "$dump")"

  if [[ "$filename" =~ ^([a-z0-9_]+)wiki-(20[0-9][0-9][01][0-9][0-3][0-9])-pages-logging\.xml\.gz$ ]]; then
    site="${BASH_REMATCH[1]}"
    dump_date="${BASH_REMATCH[2]}"
  else
    return 1
  fi

  out_date_dir="$OUT_DIR_ABS/$dump_date"
  base="${site}wiki.thanks"
  EDGE_CSV="$out_date_dir/$base.edges.csv"
  NODE_CSV="$out_date_dir/$base.nodes.csv"
  AUDIT_CSV="$out_date_dir/$base.target_resolution_audit.csv"
  return 0
}

check_csv_header() {
  local path="$1"
  local expected="$2"
  local header=""

  [[ -s "$path" ]] || return 1
  IFS= read -r header < "$path" || true
  [[ "$header" == "$expected" ]]
}

verify_outputs_or_explain() {
  local dump="$1"

  if ! expected_output_paths_for_dump "$dump"; then
    echo "could not infer expected output names from dump filename: $(basename "$dump")"
    return 1
  fi

  if ! check_csv_header "$EDGE_CSV" "site,logid,timestamp,source,target"; then
    echo "missing, empty, or invalid edges CSV: $EDGE_CSV"
    return 1
  fi

  if ! check_csv_header "$NODE_CSV" "site,id,node_type,label,creation_timestamp,creation_evidence,first_block_by_user_timestamp,sent,received"; then
    echo "missing, empty, or invalid nodes CSV: $NODE_CSV"
    return 1
  fi

  if ! check_csv_header "$AUDIT_CSV" "record_type,site,id,node_type,label,creation_timestamp_missing,creation_evidence,creation_evidence_label,sent,received,sent_to_resolved_target,sent_to_unresolved_or_ambiguous_target,received_interval_exact,received_interval_and_current_username_same,received_current_username_no_interval_conflict,received_current_username_no_creation_timestamp,received_other_resolved,received_synthetic_missing_no_identity_evidence,received_synthetic_ambiguous_multiple_active_intervals,received_synthetic_missing_current_created_after_thank,received_synthetic_missing_interval_account_created_after_thank,received_synthetic_other_unresolved,logid,timestamp,source,target,observed_target_label,target_label_subtype,target_resolution,current_name_account_id,current_name_creation_timestamp,resolved_account_creation_timestamp,interval_evidence,requires_historical_review"; then
    echo "missing or empty event audit: $AUDIT_CSV"
    return 1
  fi

  return 0
}

{
  echo "extract_thanks batch run"
  echo "Started:        $(date -Is)"
  echo "GZ_DIR:         $GZ_DIR_ABS"
  echo "OUT_DIR:        $OUT_DIR_ABS"
  echo "EXTRACT_BIN:    $EXTRACT_BIN_ABS"
  echo "BUFFER_MB:      $BUFFER_MB"
  echo "LOG_FILE:       $LOG_FILE"
  echo "Files found:    ${#DUMPS[@]}"
  echo
} >> "$LOG_FILE"

if ((${#DUMPS[@]} == 0)); then
  echo "ERROR: no *-pages-logging.xml.gz files found in $GZ_DIR_ABS" | tee -a "$LOG_FILE" >&2
  exit 1
fi

FAILED=()
FAILED_REASONS=()
SUCCEEDED=()
SKIPPED=()

for dump in "${DUMPS[@]}"; do
  start_epoch="$(date +%s)"

  if verify_outputs_or_explain "$dump" > /dev/null; then
    SKIPPED+=("$dump")
    {
      echo
      echo "SKIP:   $(date -Is)"
      echo "DUMP:   $dump"
      echo "REASON: expected output CSVs already exist and passed header checks"
      echo "EDGE:   $EDGE_CSV"
      echo "NODES:  $NODE_CSV"
      echo "AUDIT:  $AUDIT_CSV"
    } >> "$LOG_FILE"
    continue
  fi

  {
    echo
    echo "START:  $(date -Is)"
    echo "DUMP:   $dump"
    echo "CMD:    $EXTRACT_BIN_ABS --force --out-dir $OUT_DIR_ABS --buffer-mb $BUFFER_MB $dump"
  } >> "$LOG_FILE"

  if command -v /usr/bin/time > /dev/null 2>&1; then
    /usr/bin/time -f 'elapsed_seconds=%e max_rss_kb=%M' "$EXTRACT_BIN_ABS" \
      --force \
      --out-dir "$OUT_DIR_ABS" \
      --buffer-mb "$BUFFER_MB" \
      "$dump" >> "$LOG_FILE" 2>&1
    rc=$?
  else
    "$EXTRACT_BIN_ABS" \
      --force \
      --out-dir "$OUT_DIR_ABS" \
      --buffer-mb "$BUFFER_MB" \
      "$dump" >> "$LOG_FILE" 2>&1
    rc=$?
  fi

  end_epoch="$(date +%s)"
  elapsed=$((end_epoch - start_epoch))

  if ((rc != 0)); then
    FAILED+=("$dump")
    FAILED_REASONS+=("exit_code=$rc")
    {
      echo "END:    $(date -Is)"
      echo "STATUS: FAILED"
      echo "REASON: exit_code=$rc"
      echo "ELAPSED_SECONDS: $elapsed"
    } >> "$LOG_FILE"
    continue
  fi

  if ! verify_msg="$(verify_outputs_or_explain "$dump" 2>&1)"; then
    FAILED+=("$dump")
    FAILED_REASONS+=("$verify_msg")
    {
      echo "END:    $(date -Is)"
      echo "STATUS: FAILED"
      echo "REASON: $verify_msg"
      echo "ELAPSED_SECONDS: $elapsed"
    } >> "$LOG_FILE"
    continue
  fi

  SUCCEEDED+=("$dump")
  {
    echo "END:    $(date -Is)"
    echo "STATUS: OK"
    echo "ELAPSED_SECONDS: $elapsed"
  } >> "$LOG_FILE"
done

{
  echo
  echo "extract_thanks batch summary"
  echo "Finished:         $(date -Is)"
  echo "Total files:      ${#DUMPS[@]}"
  echo "Succeeded new:    ${#SUCCEEDED[@]}"
  echo "Skipped existing: ${#SKIPPED[@]}"
  echo "Failed:           ${#FAILED[@]}"
  echo "Log file:         $LOG_FILE"

  if ((${#FAILED[@]} > 0)); then
    echo
    echo "Failed files:"
    for i in "${!FAILED[@]}"; do
      echo "  - ${FAILED[$i]} :: ${FAILED_REASONS[$i]}"
    done
  fi
} | tee -a "$LOG_FILE"

if ((${#FAILED[@]} > 0)); then
  exit 1
fi

exit 0
