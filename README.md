# Wikipedia Thanks: CSV reproduction

Regenerate the analytical edge and node CSVs, the compressed audit CSVs, and the site-context CSV for the 334-site release: https://doi.org/10.5281/zenodo.22738578.

Requires Python 3.10+ with `lzma`, Bash, g++, pkg-config, libxml2, zlib, curl, gzip, GNU coreutils, and standard Unix tools.

## Download and reconstruct

From this directory, choose a new absolute output directory:

```bash
nohup bash reproduce.sh "$PWD/../reconstruction-20260601" \
  > "$PWD/../reconstruction-background.log" 2>&1 < /dev/null &
echo "Process ID: $!"
```

The script builds the extractor, downloads the specified logging and corresponding site-statistics dumps, generates the CSVs, compresses each audit, packages `wikipedia-thanks-20260601.zip`, and runs mandatory data validation. Activity is also saved in the output directory's `run.log`. A release is publication-valid only if `validate_release.py` exits successfully with zero failures.

`source_urls.txt` specifies the original logging inputs. The site-context script derives the matching `site_stats.sql.gz` URLs and uses its built-in curated site labels. Downloads undergo gzip and Wikimedia MD5 checks. Original logging dumps total 26.36 GB compressed; allow additional space for outputs. Dumps are not redistributed and must remain available or have been preserved locally.

## Use preserved dumps

To regenerate the CSVs without downloading, set paths to the preserved logging and site-statistics directories and a new output directory, then run:

```bash
LOGGING=/absolute/preserved-run/gz
SITE_STATS=/absolute/preserved-run/site_stats
RUN=/absolute/new-csv-run
mkdir "$RUN"
mkdir "$RUN/out"
bash compile.sh
bash run_extract_thanks_all.sh "$LOGGING" "$RUN/out" "$PWD/extract_thanks"
bash generate_site_metadata.sh source_urls.txt \
  --site-stats-dir "$SITE_STATS" --no-download \
  --out-csv "$RUN/out/site_metadata.csv"
python3 package_release.py "$RUN/out" source_urls.txt "$RUN/wikipedia-thanks-20260601.zip"
python3 validate_release.py "$RUN/wikipedia-thanks-20260601.zip" source_urls.txt \
  --report "$RUN/release_validation.json"
```

Use the same 334 snapshot inputs for exact reconstruction. For a later snapshot, update `source_urls.txt`, supply the corresponding dumps, and use a fresh output directory; update the curated labels in `generate_site_metadata.sh` when needed.

Each audit is an XZ/LZMA2 stream using extreme preset 9e, a 64 MiB dictionary, lc=4, lp=0, pb=0, and CRC64. The outer ZIP stores these streams without recompression. Packaging uses three compression workers by default; `package_release.py --jobs 1` reduces memory use. The validator reads the compressed audits directly. `audit_schema.py` defines the unified audit columns.

License: MIT. See [LICENSE](LICENSE).
