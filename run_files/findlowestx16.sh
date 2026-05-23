#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"
OUT="${2:-summary.tsv}"

# Model: each b_* point represents 2 GiB
GIB_PER_POINT=2
# Reference throughput in GiB/s
REF_THROUGHPUT_GIBPS=32

declare -A OFFSETS=(
  [1]="0.065695"
  [2]="0.116978"
  [3]="0.142521"
  [4]="0.164691"
  [5]="0.217523"
  [6]="0.2706"
  [7]="0.271052"
  [8]="0.349447"
  [9]="0.393878"
  [10]="0.408348"
  [11]="0.441283"
  [12]="0.489874"
  [13]="0.526172"
  [14]="0.611686"
  [15]="0.641973"
  [16]="0.689564"
  [17]="0.71286"
  [18]="0.814209"
  [19]="0.906551"
  [20]="0.951918"
  [21]="0.848147"
  [22]="0.983832"
  [23]="0.924767"
  [24]="0.961955"
  [25]="1.003399"
  [26]="1.129561"
  [27]="1.060801"
  [28]="1.116586"
  [29]="1.180734"
  [30]="1.231943"
  [31]="1.239799"
  [32]="1.272574"
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
