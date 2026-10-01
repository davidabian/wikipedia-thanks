#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  cat >&2 << 'USAGE'
Usage:
  ./generate_site_metadata.sh SOURCE_URLS.txt [options]

Example:
  ./generate_site_metadata.sh source_urls.txt

Downloads the site_stats.sql.gz files corresponding to SOURCE_URLS.txt and
writes site_metadata.csv with these columns by default:
  dump_network_site_code,actual_site_code,language_label,contributions,pages,users,active_users

The statistics are mapped as:
  ss_total_edits  -> contributions
  ss_total_pages  -> pages
  ss_users        -> users
  ss_active_users -> active_users

Options:
  --out-csv PATH        Output CSV path. Default: ./site_metadata.csv
  --base-csv PATH       Preserve labels/columns from an existing curated CSV and
                        append or overwrite the four stats columns.
                        If omitted and the output CSV already exists, the output
                        CSV is used as the base before it is replaced.
  --site-stats-dir DIR  Directory for downloaded *-site_stats.sql.gz files.
                        Default: ./site_stats
  --jobs N             Parallel downloads. Default: 3
  --no-download        Do not download; require files to already exist in DIR.
  --print-urls         Print derived site_stats.sql.gz URLs and exit.
  -h, --help           Show this help.

Environment variables:
  REQUIRE_MD5=0         Do not require md5sums files/entries. Default: 1.
  FAST_SKIP=1           Skip existing .gz files without gzip/md5 validation.
                        Default: 0.

Downloads are never rate-limited by this script.
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if [[ $# -lt 1 ]]; then
  usage
  exit 1
fi

SOURCE_URLS=$1
shift 1

OUT_CSV="site_metadata.csv"
BASE_CSV=""
SITE_STATS_DIR="site_stats"
JOBS=3
NO_DOWNLOAD=0
PRINT_URLS=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out-csv)
      [[ $# -ge 2 ]] || {
        echo "ERROR: --out-csv requires a path" >&2
        exit 1
      }
      OUT_CSV=$2
      shift 2
      ;;
    --base-csv)
      [[ $# -ge 2 ]] || {
        echo "ERROR: --base-csv requires a path" >&2
        exit 1
      }
      BASE_CSV=$2
      shift 2
      ;;
    --site-stats-dir)
      [[ $# -ge 2 ]] || {
        echo "ERROR: --site-stats-dir requires a directory" >&2
        exit 1
      }
      SITE_STATS_DIR=$2
      shift 2
      ;;
    --jobs)
      [[ $# -ge 2 ]] || {
        echo "ERROR: --jobs requires a positive integer" >&2
        exit 1
      }
      JOBS=$2
      shift 2
      ;;
    --no-download)
      NO_DOWNLOAD=1
      shift
      ;;
    --print-urls)
      PRINT_URLS=1
      shift
      ;;
    *)
      echo "ERROR: unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

REQUIRE_MD5=${REQUIRE_MD5:-1}
FAST_SKIP=${FAST_SKIP:-0}

if [[ ! -f "$SOURCE_URLS" ]]; then
  echo "ERROR: SOURCE_URLS file not found: $SOURCE_URLS" >&2
  exit 1
fi
if ! [[ "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo "ERROR: --jobs must be a positive integer" >&2
  exit 1
fi
if [[ "$REQUIRE_MD5" != "0" && "$REQUIRE_MD5" != "1" ]]; then
  echo "ERROR: REQUIRE_MD5 must be 0 or 1" >&2
  exit 1
fi
if [[ "$FAST_SKIP" != "0" && "$FAST_SKIP" != "1" ]]; then
  echo "ERROR: FAST_SKIP must be 0 or 1" >&2
  exit 1
fi
if [[ -n "$BASE_CSV" && ! -f "$BASE_CSV" ]]; then
  echo "ERROR: --base-csv file not found: $BASE_CSV" >&2
  exit 1
fi
if [[ -z "$BASE_CSV" && -f "$OUT_CSV" ]]; then
  BASE_CSV=$OUT_CSV
fi

need_cmd() {
  command -v "$1" > /dev/null 2>&1 || {
    echo "ERROR: required command not found: $1" >&2
    exit 1
  }
}

need_cmd bash
need_cmd python3
need_cmd sort
if ((!NO_DOWNLOAD && !PRINT_URLS)); then
  need_cmd curl
  need_cmd gzip
  need_cmd md5sum
  need_cmd awk
  need_cmd grep
  need_cmd xargs
fi

normalize_and_derive_urls() {
  python3 - "$SOURCE_URLS" << 'PY_DERIVE'
import re
import sys
from pathlib import Path

path = Path(sys.argv[1])
pattern = re.compile(
    r"^https://dumps\.wikimedia\.org/([A-Za-z0-9_]+wiki)/(20\d{6})/"
    r"\1-\2-pages-logging\.xml\.gz$"
)
seen = set()
for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
    line = raw.split("#", 1)[0].strip()
    if not line:
        continue
    m = pattern.match(line)
    if not m:
        raise SystemExit(
            f"ERROR: unsupported pages-logging URL on line {lineno}: {line}"
        )
    dbname, dump_date = m.group(1), m.group(2)
    url = f"https://dumps.wikimedia.org/{dbname}/{dump_date}/{dbname}-{dump_date}-site_stats.sql.gz"
    if url not in seen:
        seen.add(url)
        print(url)
if not seen:
    raise SystemExit(f"ERROR: no source URLs found in {path}")
PY_DERIVE
}

if ((PRINT_URLS)); then
  normalize_and_derive_urls | sort
  exit 0
fi

mkdir -p "$SITE_STATS_DIR"
SITE_STATS_DIR_ABS="$(cd "$SITE_STATS_DIR" && pwd)"
LOG_DIR="$SITE_STATS_DIR_ABS/.logs"
MD5_DIR="$SITE_STATS_DIR_ABS/.md5sums"
mkdir -p "$LOG_DIR" "$MD5_DIR"

download_text_file() {
  local url=$1
  local out=$2
  local tmp="${out}.part"

  if [[ -s "$out" ]]; then
    return 0
  fi

  curl -fL --retry 10 --retry-all-errors --connect-timeout 30 \
    --output "$tmp" "$url"

  mv -f "$tmp" "$out"
}

remote_md5_for_url() {
  local url=$1
  local filename prefix dir md5_url md5_file md5

  filename=${url##*/}
  prefix=${filename%-site_stats.sql.gz}
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

check_md5_if_available_or_required() {
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
  local filename final part log curl_args bad

  filename=${url##*/}
  final="$SITE_STATS_DIR_ABS/$filename"
  part="$final.part"
  log="$LOG_DIR/$filename.log"

  {
    echo "[$(date -Is)] START $url"

    if [[ ! "$url" =~ ^https://dumps\.wikimedia\.org/[A-Za-z0-9_]+wiki/20[0-9]{6}/[A-Za-z0-9_]+wiki-20[0-9]{6}-site_stats\.sql\.gz$ ]]; then
      echo "ERROR: URL does not look like a Wikimedia site_stats.sql.gz dump URL: $url" >&2
      exit 1
    fi

    if [[ ! "$filename" =~ ^[A-Za-z0-9_]+wiki-20[0-9]{6}-site_stats\.sql\.gz$ ]]; then
      echo "ERROR: filename does not match expected site_stats dump pattern: $filename" >&2
      exit 1
    fi

    if [[ "$FAST_SKIP" == "1" && -s "$final" ]]; then
      echo "[$(date -Is)] FAST_SKIP existing file: $final"
      exit 0
    fi

    if [[ -s "$final" ]]; then
      echo "[$(date -Is)] Existing file found; validating: $final"
      if gzip -t "$final" && check_md5_if_available_or_required "$url" "$final"; then
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
      -C -
      --output "$part"
    )

    echo "[$(date -Is)] Downloading/resuming to: $part"
    curl "${curl_args[@]}" "$url"

    echo "[$(date -Is)] Validating gzip: $part"
    gzip -t "$part"

    echo "[$(date -Is)] Validating MD5"
    check_md5_if_available_or_required "$url" "$part"

    mv -f "$part" "$final"
    echo "[$(date -Is)] DONE $final"
  } > "$log" 2>&1
}

export SITE_STATS_DIR_ABS LOG_DIR MD5_DIR REQUIRE_MD5 FAST_SKIP
export -f download_one download_text_file remote_md5_for_url check_md5_if_available_or_required

tmp_urls=$(mktemp)
tmp_out=$(mktemp "${OUT_CSV}.tmp.XXXXXX")
cleanup() { rm -f "$tmp_urls" "$tmp_out"; }
trap cleanup EXIT

normalize_and_derive_urls | sort > "$tmp_urls"

n_urls=$(wc -l < "$tmp_urls" | tr -d ' ')
if [[ "$n_urls" == "0" ]]; then
  echo "ERROR: no site_stats URLs derived from $SOURCE_URLS" >&2
  exit 1
fi

if ((!NO_DOWNLOAD)); then
  echo "site_stats URLs: $n_urls"
  echo "site_stats directory: $SITE_STATS_DIR_ABS"
  echo "Parallel downloads: $JOBS"
  echo "REQUIRE_MD5: $REQUIRE_MD5"
  echo "FAST_SKIP: $FAST_SKIP"
  echo "Logs: $LOG_DIR"
  echo

  if xargs -a "$tmp_urls" -r -n 1 -P "$JOBS" bash -c 'download_one "$1"' _; then
    echo
    echo "All site_stats downloads completed successfully."
  else
    echo
    echo "ERROR: at least one site_stats download failed. Inspect logs in: $LOG_DIR" >&2
    grep -RilE '(^ERROR:|^curl:|^gzip:|not in gzip format|unexpected end of file|The requested URL returned error|Failed writing body|Connection timed out|Operation timed out|MD5 mismatch)' "$LOG_DIR" 2> /dev/null | tail -20 >&2 || true
    exit 1
  fi
fi

python3 - "$SOURCE_URLS" "$SITE_STATS_DIR_ABS" "$tmp_out" "$BASE_CSV" << 'PY_GENERATE'
import csv
import gzip
import re
import sys
from pathlib import Path

source_urls_path = Path(sys.argv[1])
site_stats_dir = Path(sys.argv[2])
out_path = Path(sys.argv[3])
base_csv_path = Path(sys.argv[4]) if sys.argv[4] else None

REQUIRED_STATS = {
    "ss_total_edits": "contributions",
    "ss_total_pages": "pages",
    "ss_users": "users",
    "ss_active_users": "active_users",
}
OUT_STAT_COLUMNS = list(REQUIRED_STATS.values())
DEFAULT_FIELDNAMES = [
    "dump_network_site_code",
    "actual_site_code",
    "language_label",
    *OUT_STAT_COLUMNS,
]

BUILTIN_LABELS_TSV = (
    "en\ten\tEnglish\n"
    "de\tde\tGerman\n"
    "fr\tfr\tFrench\n"
    "es\tes\tSpanish\n"
    "ja\tja\tJapanese\n"
    "ru\tru\tRussian\n"
    "pt\tpt\tPortuguese\n"
    "it\tit\tItalian\n"
    "zh\tzh\tChinese\n"
    "fa\tfa\tPersian\n"
    "pl\tpl\tPolish\n"
    "ar\tar\tArabic\n"
    "nl\tnl\tDutch\n"
    "he\the\tHebrew\n"
    "uk\tuk\tUkrainian\n"
    "tr\ttr\tTurkish\n"
    "id\tid\tIndonesian\n"
    "cs\tcs\tCzech\n"
    "ko\tko\tKorean\n"
    "sv\tsv\tSwedish\n"
    "vi\tvi\tVietnamese\n"
    "th\tth\tThai\n"
    "hu\thu\tHungarian\n"
    "fi\tfi\tFinnish\n"
    "simple\tsimple\tSimple English\n"
    "ca\tca\tCatalan\n"
    "hi\thi\tHindi\n"
    "no\tno\tNorwegian\n"
    "el\tel\tGreek\n"
    "bn\tbn\tBengali/Bangla\n"
    "ro\tro\tRomanian\n"
    "sr\tsr\tSerbian\n"
    "uz\tuz\tUzbek\n"
    "ms\tms\tMalay\n"
    "da\tda\tDanish\n"
    "bg\tbg\tBulgarian\n"
    "az\taz\tAzerbaijani\n"
    "hy\thy\tArmenian\n"
    "sk\tsk\tSlovak\n"
    "et\tet\tEstonian\n"
    "hr\thr\tCroatian\n"
    "lt\tlt\tLithuanian\n"
    "ha\tha\tHausa\n"
    "eo\teo\tEsperanto\n"
    "eu\teu\tBasque\n"
    "sl\tsl\tSlovenian/Slovene\n"
    "ta\tta\tTamil\n"
    "lv\tlv\tLatvian\n"
    "ka\tka\tGeorgian\n"
    "zh_yue\tzh-yue\tCantonese\n"
    "ml\tml\tMalayalam\n"
    "be\tbe\tBelarusian\n"
    "ur\tur\tUrdu\n"
    "kk\tkk\tKazakh\n"
    "kn\tkn\tKannada\n"
    "gl\tgl\tGalician\n"
    "sq\tsq\tAlbanian\n"
    "af\taf\tAfrikaans\n"
    "mk\tmk\tMacedonian\n"
    "mn\tmn\tMongolian\n"
    "arz\tarz\tEgyptian Arabic\n"
    "te\tte\tTelugu\n"
    "sw\tsw\tSwahili\n"
    "sh\tsh\tSerbo-Croatian\n"
    "mr\tmr\tMarathi\n"
    "ceb\tceb\tCebuano\n"
    "la\tla\tLatin\n"
    "my\tmy\tBurmese\n"
    "bs\tbs\tBosnian\n"
    "tl\ttl\tTagalog\n"
    "is\tis\tIcelandic\n"
    "ckb\tckb\tCentral/Sorani Kurdish\n"
    "nn\tnn\tNorwegian Nynorsk\n"
    "be_x_old\tbe-tarask\tBelarusian (Taraškievica/Classical)\n"
    "cy\tcy\tWelsh\n"
    "br\tbr\tBreton\n"
    "pa\tpa\tPunjabi\n"
    "ne\tne\tNepali\n"
    "zh_min_nan\tzh-min-nan\tSouthern Min/Minnan\n"
    "azb\tazb\tSouth Azerbaijani\n"
    "sa\tsa\tSanskrit\n"
    "ast\tast\tAsturian\n"
    "ku\tku\tKurdish\n"
    "jv\tjv\tJavanese\n"
    "oc\toc\tOccitan\n"
    "war\twar\tWaray\n"
    "si\tsi\tSinhala\n"
    "fy\tfy\tWest Frisian\n"
    "as\tas\tAssamese\n"
    "sco\tsco\tScots\n"
    "tg\ttg\tTajik\n"
    "ig\tig\tIgbo\n"
    "wuu\twuu\tWu\n"
    "ky\tky\tKyrgyz\n"
    "yo\tyo\tYoruba\n"
    "tt\ttt\tTatar\n"
    "km\tkm\tKhmer\n"
    "als\tals\tAlemannic\n"
    "lb\tlb\tLuxembourgish\n"
    "so\tso\tSomali\n"
    "an\tan\tAragonese\n"
    "ban\tban\tBalinese\n"
    "ga\tga\tIrish\n"
    "gu\tgu\tGujarati\n"
    "rw\trw\tKinyarwanda\n"
    "ba\tba\tBashkir\n"
    "cv\tcv\tChuvash\n"
    "dz\tdz\tDzongkha\n"
    "ce\tce\tChechen\n"
    "zu\tzu\tZulu\n"
    "lmo\tlmo\tLombard\n"
    "bar\tbar\tBavarian\n"
    "nds\tnds\tLow German\n"
    "zh_classical\tzh-classical\tClassical/Literary Chinese\n"
    "bcl\tbcl\tCentral Bikol\n"
    "am\tam\tAmharic\n"
    "io\tio\tIdo\n"
    "scn\tscn\tSicilian\n"
    "min\tmin\tMinangkabau\n"
    "ff\tff\tFula\n"
    "ps\tps\tPashto\n"
    "szl\tszl\tSilesian\n"
    "pnb\tpnb\tWestern Punjabi\n"
    "kaa\tkaa\tKarakalpak/Kara-Kalpak\n"
    "ary\tary\tMoroccan Arabic\n"
    "pms\tpms\tPiedmontese\n"
    "crh\tcrh\tCrimean Tatar\n"
    "ht\tht\tHaitian Creole\n"
    "mg\tmg\tMalagasy\n"
    "fo\tfo\tFaroese\n"
    "ang\tang\tOld English\n"
    "su\tsu\tSundanese\n"
    "or\tor\tOdia\n"
    "qu\tqu\tQuechua\n"
    "dag\tdag\tDagbani\n"
    "hif\thif\tFiji Hindi\n"
    "lg\tlg\tLuganda/Ganda\n"
    "lij\tlij\tLigurian\n"
    "mad\tmad\tMadurese\n"
    "rue\true\tRusyn\n"
    "bh\tbh\tBhojpuri\n"
    "ab\tab\tAbkhaz/Abkhazian\n"
    "ia\tia\tInterlingua\n"
    "ace\tace\tAcehnese\n"
    "tw\ttw\tTwi\n"
    "sah\tsah\tYakut\n"
    "tn\ttn\tTswana\n"
    "sat\tsat\tSantali\n"
    "sd\tsd\tSindhi\n"
    "yi\tyi\tYiddish\n"
    "mt\tmt\tMaltese\n"
    "hyw\thyw\tWestern Armenian\n"
    "ee\tee\tEwe\n"
    "iu\tiu\tInuktitut\n"
    "li\tli\tLimburgish\n"
    "xmf\txmf\tMingrelian\n"
    "bjn\tbjn\tBanjarese/Banjar\n"
    "frr\tfrr\tNorth/Northern Frisian\n"
    "vec\tvec\tVenetian\n"
    "mzn\tmzn\tMazanderani\n"
    "cu\tcu\tOld Church Slavonic/Church Slavic\n"
    "chr\tchr\tCherokee\n"
    "co\tco\tCorsican\n"
    "cdo\tcdo\tEastern Min/Mindong\n"
    "nds_nl\tnds-nl\tDutch Low Saxon/Low Saxon\n"
    "bat_smg\tbat-smg\tSamogitian\n"
    "vo\tvo\tVolapük\n"
    "ay\tay\tAymara\n"
    "eml\teml\tEmilian–Romagnol/Emiliano-Romagnolo\n"
    "fur\tfur\tFriulian\n"
    "lfn\tlfn\tLingua Franca Nova\n"
    "gn\tgn\tGuarani\n"
    "hak\thak\tHakka/Hakka Chinese\n"
    "haw\thaw\tHawaiian\n"
    "ie\tie\tInterlingue\n"
    "pam\tpam\tKapampangan/Pampanga\n"
    "sc\tsc\tSardinian\n"
    "tk\ttk\tTurkmen\n"
    "lld\tlld\tLadin\n"
    "zgh\tzgh\tMoroccan Amazigh/Standard Moroccan Tamazight\n"
    "hsb\thsb\tUpper Sorbian\n"
    "ilo\tilo\tIlocano/Iloko\n"
    "dsb\tdsb\tLower Sorbian\n"
    "xh\txh\tXhosa\n"
    "tcy\ttcy\tTulu\n"
    "kw\tkw\tCornish\n"
    "frp\tfrp\tFranco-Provençal/Arpitan\n"
    "gan\tgan\tGan\n"
    "lo\tlo\tLao\n"
    "nap\tnap\tNeapolitan\n"
    "bo\tbo\tTibetan\n"
    "tum\ttum\tTumbuka\n"
    "wa\twa\tWalloon\n"
    "os\tos\tOssetian\n"
    "diq\tdiq\tZazaki/Dimli\n"
    "ug\tug\tUyghur\n"
    "bi\tbi\tBislama\n"
    "bug\tbug\tBuginese\n"
    "cr\tcr\tCree\n"
    "got\tgot\tGothic\n"
    "mhr\tmhr\tMeadow/Eastern Mari\n"
    "pcd\tpcd\tPicard\n"
    "rm\trm\tRomansh\n"
    "gd\tgd\tScottish Gaelic\n"
    "gor\tgor\tGorontalo\n"
    "lad\tlad\tJudaeo-Spanish/Ladino\n"
    "kab\tkab\tKabyle\n"
    "dv\tdv\tMaldivian/Divehi\n"
    "vls\tvls\tWest Flemish\n"
    "av\tav\tAvar/Avaric\n"
    "mi\tmi\tMāori\n"
    "om\tom\tOromo\n"
    "rmy\trmy\tRomani\n"
    "tly\ttly\tTalysh\n"
    "vep\tvep\tVeps\n"
    "zea\tzea\tZeelandic\n"
    "mai\tmai\tMaithili\n"
    "fat\tfat\tFante/Fanti\n"
    "rn\trn\tKirundi/Rundi\n"
    "kv\tkv\tKomi\n"
    "nqo\tnqo\tN'Ko\n"
    "se\tse\tNorthern Sámi/Sami\n"
    "pdc\tpdc\tPennsylvania Dutch/German\n"
    "kcg\tkcg\tTyap\n"
    "roa_rup\troa-rup\tAromanian\n"
    "gv\tgv\tManx\n"
    "mni\tmni\tMeitei/Manipuri\n"
    "mwl\tmwl\tMirandese\n"
    "pap\tpap\tPapiamento\n"
    "shn\tshn\tShan\n"
    "glk\tglk\tGilaki\n"
    "avk\tavk\tKotava\n"
    "ss\tss\tSwazi/Swati\n"
    "udm\tudm\tUdmurt\n"
    "ext\text\tExtremaduran\n"
    "gom\tgom\tKonkani/Goan Konkani\n"
    "gpe\tgpe\tGhanaian Pidgin\n"
    "inh\tinh\tIngush\n"
    "csb\tcsb\tKashubian\n"
    "nv\tnv\tNavajo\n"
    "st\tst\tSotho/Southern Sotho\n"
    "ks\tks\tKashmiri\n"
    "ady\tady\tAdyghe\n"
    "ami\tami\tAmis\n"
    "ik\tik\tIñupiaq/Inupiaq\n"
    "jam\tjam\tJamaican Patois/Creole English\n"
    "kg\tkg\tKongo\n"
    "lez\tlez\tLezgian/Lezghian\n"
    "mdf\tmdf\tMoksha\n"
    "nah\tnah\tNahuatl\n"
    "nia\tnia\tNias\n"
    "pfl\tpfl\tPalatine German\n"
    "szy\tszy\tSakizaya\n"
    "shi\tshi\tShilha/Tachelhit\n"
    "tpi\ttpi\tTok Pisin\n"
    "za\tza\tZhuang\n"
    "ksh\tksh\tRipuarian/Colognian\n"
    "wo\two\tWolof\n"
    "arc\tarc\tAramaic\n"
    "bpy\tbpy\tBishnupriya Manipuri/Bishnupriya\n"
    "bxr\tbxr\tBuryat/Russia Buriat\n"
    "ch\tch\tChamorro\n"
    "cbk_zam\tcbk-zam\tChavacano\n"
    "guw\tguw\tGun\n"
    "ltg\tltg\tLatgalian\n"
    "mnw\tmnw\tMon\n"
    "pcm\tpcm\tNigerian Pidgin\n"
    "skr\tskr\tSaraiki\n"
    "tet\ttet\tTetum\n"
    "fiu_vro\tfiu-vro\tVõro\n"
    "map_bms\tmap-bms\tBanyumasan\n"
    "din\tdin\tDinka\n"
    "dty\tdty\tDoteli\n"
    "myv\tmyv\tErzya\n"
    "fon\tfon\tFon\n"
    "bbc\tbbc\tToba Batak/Batak Toba\n"
    "awa\tawa\tAwadhi\n"
    "kl\tkl\tGreenlandic\n"
    "lbe\tlbe\tLak\n"
    "new\tnew\tNewar/Newari\n"
    "pag\tpag\tPangasinan\n"
    "alt\talt\tSouthern Altai\n"
    "roa_tara\troa-tara\tTarantino\n"
    "ts\tts\tTsonga\n"
    "tyv\ttyv\tTuvan/Tuvinian\n"
    "ny\tny\tChewa/Nyanja\n"
    "gur\tgur\tGurene/Frafra\n"
    "mrj\tmrj\tHill/Western Mari\n"
    "jbo\tjbo\tLojban\n"
    "pih\tpih\tNorfuk\n"
    "nrm\tnrm\tNorman\n"
    "sn\tsn\tShona\n"
    "gag\tgag\tGagauz\n"
    "smn\tsmn\tInari Sámi/Sami\n"
    "kbd\tkbd\tKabardian\n"
    "kbp\tkbp\tKabiye\n"
    "krc\tkrc\tKarachay-Balkar\n"
    "ki\tki\tKikuyu\n"
    "ln\tln\tLingala\n"
    "olo\tolo\tLivvi-Karelian\n"
    "nso\tnso\tNorthern Sotho\n"
    "blk\tblk\tPa'O\n"
    "sm\tsm\tSamoan\n"
    "to\tto\tTongan\n"
    "tay\ttay\tAtayal\n"
    "nov\tnov\tNovial\n"
    "pnt\tpnt\tPontic\n"
    "stq\tstq\tSaterland Frisian\n"
    "ve\tve\tVenda\n"
    "atj\tatj\tAtikamekw\n"
    "chy\tchy\tCheyenne\n"
    "fj\tfj\tFijian\n"
    "srn\tsrn\tSranan Tongo\n"
    "ty\tty\tTahitian\n"
    "ti\tti\tTigrinya\n"
    "bm\tbm\tBambara\n"
    "dga\tdga\tDagaare/Southern Dagaare\n"
    "gcr\tgcr\tGuianan Creole\n"
    "koi\tkoi\tKomi-Permyak\n"
    "pwn\tpwn\tPaiwan\n"
    "sg\tsg\tSango\n"
    "trv\ttrv\tSeediq/Taroko\n"
    "guc\tguc\tWayuu\n"
    "anp\tanp\tAngika\n"
    "xal\txal\tKalmyk\n"
    "pi\tpi\tPali\n"
    "test\ttest\tTest Wikipedia\n"
    "test2\ttest2\tTest2 Wikipedia\n"
    "cho\tcho\tChoctaw\n"
    "lrc\tlrc\tNorthern Luri\n"
    "hz\thz\tHerero\n"
    "ak\tak\tAkan\n"
    "aa\taa\tAfar\n"
    "na\tna\tNauruan/Nauru\n"
    "ho\tho\tHiri Motu\n"
    "ii\tii\tSichuan Yi\n"
    "kj\tkj\tKwanyama\n"
    "kr\tkr\tKanuri\n"
    "mh\tmh\tMarshallese\n"
    "mus\tmus\tMuscogee\n"
    "ng\tng\tNdonga\n"
    "nostalgia\tnostalgia\tNostalgia Wikipedia\n"
    "ten\tten\tWikipedia 10"
)

builtin_rows = []
for line in BUILTIN_LABELS_TSV.splitlines():
    site, actual, label = line.split("\t", 2)
    builtin_rows.append(
        {
            "dump_network_site_code": site,
            "actual_site_code": actual,
            "language_label": label,
        }
    )
BUILTIN_BY_SITE = {r["dump_network_site_code"]: r for r in builtin_rows}


def die(msg: str) -> None:
    raise SystemExit(f"ERROR: {msg}")


def normalize_source_urls(path: Path):
    pattern = re.compile(
        r"^https://dumps\.wikimedia\.org/([A-Za-z0-9_]+wiki)/(20\d{6})/"
        r"\1-\2-pages-logging\.xml\.gz$"
    )
    out = []
    seen = set()
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        m = pattern.match(line)
        if not m:
            die(f"unsupported pages-logging URL on line {lineno}: {line}")
        dbname, dump_date = m.group(1), m.group(2)
        site = dbname[:-4]
        key = (site, dbname, dump_date)
        if key not in seen:
            seen.add(key)
            out.append({"site": site, "site_dbname": dbname, "dump_date": dump_date})
    if not out:
        die(f"no source URLs found in {path}")
    return out


def split_sql_tuple_values(s: str):
    values = []
    cur = []
    i = 0
    in_string = False
    while i < len(s):
        ch = s[i]
        if in_string:
            if ch == "\\" and i + 1 < len(s):
                cur.append(s[i + 1])
                i += 2
                continue
            if ch == "'":
                if i + 1 < len(s) and s[i + 1] == "'":
                    cur.append("'")
                    i += 2
                    continue
                in_string = False
                i += 1
                continue
            cur.append(ch)
            i += 1
            continue
        if ch == "'":
            in_string = True
            i += 1
            continue
        if ch == ",":
            values.append("".join(cur).strip())
            cur = []
            i += 1
            continue
        cur.append(ch)
        i += 1
    if in_string:
        die("unterminated SQL string while parsing site_stats row")
    values.append("".join(cur).strip())
    return values


def values_tuples_after_values(sql: str):
    marker_match = re.search(r"\bVALUES\b", sql, flags=re.IGNORECASE)
    if not marker_match:
        die("INSERT INTO site_stats has no VALUES clause")
    # All site_stats columns are integer counters. Reject unsupported SQL
    # instead of silently accepting a partial tuple or partial statement.
    payload = sql[marker_match.end() :].strip()
    if payload.endswith(";"):
        payload = payload[:-1].rstrip()
    if not re.fullmatch(r"\([^()]*\)(?:\s*,\s*\([^()]*\))*", payload):
        die("unsupported or malformed VALUES tuples in site_stats INSERT")
    return re.findall(r"\(([^()]*)\)", payload)


def parse_site_stats_sql_gz(path: Path):
    if not path.is_file() or path.stat().st_size == 0:
        die(f"missing or empty site_stats file: {path}")
    try:
        with gzip.open(path, "rt", encoding="utf-8", errors="replace") as fh:
            sql = fh.read()
    except Exception as exc:
        die(f"could not read gzip SQL file {path}: {exc}")

    create_match = re.search(
        r"CREATE\s+TABLE\s+`?site_stats`?\s*\((.*?)\)\s*(?:ENGINE|;)",
        sql,
        flags=re.IGNORECASE | re.DOTALL,
    )
    if not create_match:
        die(f"could not find CREATE TABLE site_stats in {path}")

    columns = []
    for raw_line in create_match.group(1).splitlines():
        line = raw_line.strip()
        if not line.startswith("`"):
            continue
        m = re.match(r"`([^`]+)`", line)
        if m:
            columns.append(m.group(1))
    if not columns:
        die(f"could not parse site_stats columns in {path}")

    missing_cols = [c for c in REQUIRED_STATS if c not in columns]
    if missing_cols:
        die(f"missing required site_stats columns in {path}: {', '.join(missing_cols)}")

    insert_matches = list(
        re.finditer(
            r"INSERT\s+INTO\s+`?site_stats`?\b.*?;",
            sql,
            flags=re.IGNORECASE | re.DOTALL,
        )
    )
    if not insert_matches:
        die(f"could not find INSERT INTO site_stats in {path}")

    # MediaWiki sums every site_stats row. A NULL shard value contributes
    # nothing, including ss_active_users in rows other than the first.
    # Keep the existing requirement that each released field has evidence:
    # a column with only NULL values is not silently published as zero.
    totals = {col: 0 for col in REQUIRED_STATS}
    non_null = {col: 0 for col in REQUIRED_STATS}
    seen_row_ids = set()
    for insert_match in insert_matches:
        for tuple_text in values_tuples_after_values(insert_match.group(0)):
            values = split_sql_tuple_values(tuple_text)
            if len(values) != len(columns):
                die(
                    f"site_stats column/value length mismatch in {path}: {len(columns)} columns, {len(values)} values"
                )
            row = dict(zip(columns, values))
            row_id = row.get("ss_row_id", "")
            if not re.fullmatch(r"[0-9]+", row_id):
                die(f"ss_row_id is not a non-negative integer in {path}: {row_id!r}")
            row_id = int(row_id)
            if row_id in seen_row_ids:
                die(f"duplicate site_stats row ID {row_id} in {path}")
            seen_row_ids.add(row_id)
            for source_col in REQUIRED_STATS:
                val = row[source_col].strip()
                if val.upper() == "NULL":
                    continue
                if not re.fullmatch(r"[0-9]+", val):
                    die(
                        f"{source_col} is not a non-negative integer in {path}: {val!r}"
                    )
                totals[source_col] += int(val)
                non_null[source_col] += 1

    out = {}
    for source_col, out_col in REQUIRED_STATS.items():
        if not non_null[source_col]:
            die(f"{source_col} has no non-NULL value in {path}")
        out[out_col] = str(totals[source_col])
    return out


def fallback_row_for_site(site: str):
    if site in BUILTIN_BY_SITE:
        return dict(BUILTIN_BY_SITE[site])
    label = ""
    try:
        from babel import Locale

        label = Locale.parse(site.replace("_", "-"), sep="-").english_name
    except Exception:
        pass
    if not label:
        try:
            import pycountry

            lang = pycountry.languages.get(alpha_2=site) or pycountry.languages.get(
                alpha_3=site
            )
            if lang is not None:
                label = getattr(lang, "name", "")
        except Exception:
            pass
    if not label:
        die(
            f"no language_label available for site {site}; pass --base-csv with a curated label"
        )
    return {
        "dump_network_site_code": site,
        "actual_site_code": site.replace("_", "-"),
        "language_label": label,
    }


def read_base_csv(path: Path):
    with path.open("r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh)
        if reader.fieldnames is None:
            die(f"base CSV has no header: {path}")
        rows = list(reader)
        return list(reader.fieldnames), rows


def row_site(row):
    for key in ("dump_network_site_code", "site", "site_code", "language_code", "code"):
        val = row.get(key, "")
        if val:
            return val.strip()
    for key in ("site_dbname", "dbname", "wiki_dbname", "database"):
        val = row.get(key, "")
        if val and val.endswith("wiki"):
            return val[:-4]
    return ""


sources = normalize_source_urls(source_urls_path)
source_by_site = {row["site"]: row for row in sources}
if len(source_by_site) != len(sources):
    die("duplicate site values found in source URLs")

stats_by_site = {}
for row in sources:
    stat_path = (
        site_stats_dir / f"{row['site_dbname']}-{row['dump_date']}-site_stats.sql.gz"
    )
    stats_by_site[row["site"]] = parse_site_stats_sql_gz(stat_path)

if base_csv_path is not None:
    fieldnames, rows = read_base_csv(base_csv_path)
    for required in ("dump_network_site_code", "actual_site_code", "language_label"):
        if required not in fieldnames:
            fieldnames.append(required)
    for col in OUT_STAT_COLUMNS:
        if col not in fieldnames:
            fieldnames.append(col)
else:
    fieldnames = list(DEFAULT_FIELDNAMES)
    rows = []

seen_sites = set()
for row in rows:
    site = row_site(row)
    if site in stats_by_site:
        base_label = fallback_row_for_site(site)
        for key in ("dump_network_site_code", "actual_site_code", "language_label"):
            if not row.get(key, ""):
                row[key] = base_label[key]
        row.update(stats_by_site[site])
        seen_sites.add(site)

for source_row in sources:
    site = source_row["site"]
    if site in seen_sites:
        continue
    new_row = {col: "" for col in fieldnames}
    new_row.update(fallback_row_for_site(site))
    new_row.update(stats_by_site[site])
    rows.append(new_row)
    seen_sites.add(site)

for row in rows:
    for col in OUT_STAT_COLUMNS:
        val = row.get(col, "")
        if val and not re.fullmatch(r"[0-9]+", val):
            die(
                f"output column {col} is not a non-negative integer for row {row}: {val!r}"
            )
    site = row.get("dump_network_site_code", "")
    if site in source_by_site:
        for key in ("dump_network_site_code", "actual_site_code", "language_label"):
            if not row.get(key, ""):
                die(f"missing {key} for source site {site}")

missing_sites = sorted(set(source_by_site) - seen_sites)
if missing_sites:
    die("source sites missing from output CSV: " + ", ".join(missing_sites[:50]))

out_path.parent.mkdir(parents=True, exist_ok=True)
with out_path.open("w", encoding="utf-8", newline="") as fh:
    writer = csv.DictWriter(
        fh, fieldnames=fieldnames, extrasaction="ignore", lineterminator="\n"
    )
    writer.writeheader()
    for row in rows:
        writer.writerow({col: row.get(col, "") for col in fieldnames})

print(f"Wrote {len(rows)} rows to {out_path}", file=sys.stderr)
print(f"Stats columns: {', '.join(OUT_STAT_COLUMNS)}", file=sys.stderr)
PY_GENERATE

mv -f "$tmp_out" "$OUT_CSV"
trap - EXIT
rm -f "$tmp_urls"
