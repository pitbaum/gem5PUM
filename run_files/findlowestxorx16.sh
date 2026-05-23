#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"
OUT="${2:-summary.tsv}"

# Model: each b_* point represents 2 GiB
GIB_PER_POINT=1
# Reference throughput in GiB/s
REF_THROUGHPUT_GIBPS=32
declare -A OFFSETS=(
  [1]="0.025833"
  [2]="0.03824"
  [3]="0.041808"
  [4]="0.052695"
  [5]="0.061235"
  [6]="0.083111"
  [7]="0.077597"
  [8]="0.091108"
  [9]="0.116221"
  [10]="0.128378"
  [11]="0.127956"
  [12]="0.146117"
  [13]="0.138223"
  [14]="0.149637"
  [15]="0.158831"
  [16]="0.195888"
  [17]="0.191196"
  [18]="0.233131"
  [19]="0.240697"
  [20]="0.251129"
  [21]="0.265183"
  [22]="0.275561"
  [23]="0.284798"
  [24]="0.297022"
  [25]="0.307754"
  [26]="0.32149"
  [27]="0.330477"
  [28]="0.342438"
  [29]="0.354933"
  [30]="0.367339"
  [31]="0.378394"
  [32]="0.355433"
)

BYTES_PER_POINT=$(( GIB_PER_POINT * 1024 * 1024 * 1024 ))

printf "b_folder\tpercent_range\n" > "$OUT"

find "$ROOT" -mindepth 1 -maxdepth 1 -type d -name 'b_*' | sort -V |
while IFS= read -r bdir; do
  bname="$(basename "$bdir")"
  [[ "$bname" =~ ^b_([0-9]+)$ ]] || continue
  B="${BASH_REMATCH[1]}"   # 1..32

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

        if (diff == 0) { exact_found = 1; exact_k = k }

        if (diff > 0) {
          if (best_pos_diff == "" || diff < best_pos_diff) {
            best_pos_diff = diff; best_pos_k = k
          }
        }
        if (diff < 0) {
          if (best_neg_diff == "" || diff > best_neg_diff) {
            best_neg_diff = diff; best_neg_k = k
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

  total_bytes="$(awk -v b="$B" -v bpp="$BYTES_PER_POINT" 'BEGIN { printf "%.0f", b * bpp }')"

  percents=()
  while IFS=$'\t' read -r kind best_k; do
    # This is still your original mapping. If k is NOT 64-byte units, this must change.
    bytes_value=$(( best_k * 64 ))

    percent="$(awk -v bytes="$bytes_value" -v total="$total_bytes" '
      BEGIN { printf "%.0f", (bytes / total) * 100 }
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
