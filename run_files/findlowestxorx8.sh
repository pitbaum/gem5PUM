#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"                  # parent directory containing b_* folders
OUT="${2:-summary.tsv}"         # output file

# Model: each b_* point represents 0.5 GiB
GIB_PER_POINT="0.5"
# Reference throughput in GiB/s
REF_THROUGHPUT_GIBPS="32"
declare -A OFFSETS=(
  [1]="0.006007"
  [2]="0.0061"
  [3]="0.006191"
  [4]="0.006359"
  [5]="0.006434"
  [6]="0.006551"
  [7]="0.00674"
  [8]="0.007478"
  [9]="0.007584"
  [10]="0.007582"
  [11]="0.007671"
  [12]="0.007769"
  [13]="0.007784"
  [14]="0.008158"
  [15]="0.008081"
  [16]="0.008248"
  [17]="0.009087"
  [18]="0.009981"
  [19]="0.01089"
  [20]="0.011833"
  [21]="0.012666"
  [22]="0.013611"
  [23]="0.014552"
  [24]="0.015523"
  [25]="0.015555"
  [26]="0.01569"
  [27]="0.015819"
  [28]="0.015959"
  [29]="0.015978"
  [30]="0.01609"
  [31]="0.016273"
  [32]="0.016403"
  [33]="0.016388"
  [34]="0.016507"
  [35]="0.016665"
  [36]="0.016781"
  [37]="0.016781"
  [38]="0.016844"
  [39]="0.017127"
  [40]="0.017469"
  [41]="0.017691"
  [42]="0.017903"
  [43]="0.018128"
  [44]="0.018464"
  [45]="0.018944"
  [46]="0.019323"
  [47]="0.019596"
  [48]="0.019765"
  [49]="0.020011"
  [50]="0.020294"
  [51]="0.020586"
  [52]="0.020923"
  [53]="0.021182"
  [54]="0.021575"
  [55]="0.022389"
  [56]="0.022305"
  [57]="0.022888"
  [58]="0.023386"
  [59]="0.02388"
  [60]="0.024698"
  [61]="0.025178"
  [62]="0.025085"
  [63]="0.025539"
  [64]="0.026151"
)

printf "b_folder\tpercent_range\n" > "$OUT"

find "$ROOT" -mindepth 1 -maxdepth 1 -type d -name 'b_*' | sort -V |
while IFS= read -r bdir; do
  bname="$(basename "$bdir")"

  [[ "$bname" =~ ^b_([0-9]+)$ ]] || continue
  B="${BASH_REMATCH[1]}"

  OFFSET="${OFFSETS[$B]:-}"
  if [[ -z "${OFFSET:-}" ]]; then
    echo "Warning: no offset configured for $bname, skipping" >&2
    continue
  fi

  btmp="$(mktemp)"
  trap 'rm -f "$btmp"' RETURN

  find "$bdir" -type f -name "stats.txt" -print0 |
  while IFS= read -r -d '' f; do
    folder="$(basename "$(dirname "$f")")"
    simSeconds="$(awk '$1=="simSeconds"{print $2; exit}' "$f" || true)"
    [[ -n "${simSeconds:-}" ]] || continue

    if [[ "$folder" =~ ^k_([0-9]+)$ ]]; then
      k_num="${BASH_REMATCH[1]}"
      printf "%s\t%s\n" "$k_num" "$simSeconds" >> "$btmp"
    fi
  done

  if [[ ! -s "$btmp" ]]; then
    rm -f "$btmp"
    trap - RETURN
    continue
  fi

  matches="$(
    awk -F'\t' -v offset="$OFFSET" -v b="$B" -v gibpp="$GIB_PER_POINT" -v thr="$REF_THROUGHPUT_GIBPS" '
      BEGIN {
        target = (b * gibpp) / thr
        exact_found = 0
        best_pos_diff = ""
        best_neg_diff = ""
      }
      {
        k = $1
        sim = $2 + 0
        diff = target - (sim + offset)

        if (diff == 0) {
          exact_found = 1
          exact_k = k
        }

        if (diff > 0) {
          if (best_pos_diff == "" || diff < best_pos_diff) {
            best_pos_diff = diff
            best_pos_k = k
          }
        }

        if (diff < 0) {
          if (best_neg_diff == "" || diff > best_neg_diff) {
            best_neg_diff = diff
            best_neg_k = k
          }
        }
      }
      END {
        if (exact_found) {
          printf "EXACT\t%s\n", exact_k
        } else {
          if (best_pos_diff != "") printf "POS\t%s\n", best_pos_k
          if (best_neg_diff != "") printf "NEG\t%s\n", best_neg_k
        }
      }
    ' "$btmp"
  )"

  rm -f "$btmp"
  trap - RETURN

  [[ -n "$matches" ]] || continue

  total_bytes="$(awk -v b="$B" -v gibpp="$GIB_PER_POINT" '
    BEGIN { printf "%.0f", b * gibpp * 1024 * 1024 * 1024 }
  ')"

  percents=()
  while IFS=$'\t' read -r kind best_k; do
    bytes_value=$(( best_k * 64 ))
    percent="$(awk -v bytes="$bytes_value" -v total="$total_bytes" '
      BEGIN {
        printf "%.0f", (bytes / total) * 100
      }
    ')"
    percents+=("$percent")
  done <<< "$matches"

  if [[ "${#percents[@]}" -eq 1 ]]; then
    range="${percents[0]}"
  else
    p1="${percents[0]}"
    p2="${percents[1]}"
    if (( p1 < p2 )); then
      range="${p1}-${p2}"
    else
      range="${p2}-${p1}"
    fi
  fi

printf "%s\t%s\n" "$bname" "$range" >> "$OUT"
done