#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"                  # parent directory containing b_* folders
OUT="${2:-summary.tsv}"         # output file
declare -A OFFSETS=(
  [1]="0.012917"
  [2]="0.018969"
  [3]="0.024376"
  [4]="0.030551"
  [5]="0.031325"
  [6]="0.039717"
  [7]="0.041592"
  [8]="0.048174"
  [9]="0.053539"
  [10]="0.061569"
  [11]="0.065346"
  [12]="0.071641"
  [13]="0.069888"
  [14]="0.076736"
  [15]="0.083701"
  [16]="0.095361"
  [17]="0.094164"
  [18]="0.110522"
  [19]="0.107661"
  [20]="0.114983"
  [21]="0.117503"
  [22]="0.123504"
  [23]="0.130391"
  [24]="0.138888"
  [25]="0.139278"
  [26]="0.14655"
  [27]="0.152857"
  [28]="0.158827"
  [29]="0.160577"
  [30]="0.165877"
  [31]="0.171224"
  [32]="0.179303"
  [33]="0.181877"
  [34]="0.187203"
  [35]="0.194177"
  [36]="0.203171"
  [37]="0.203171"
  [38]="0.206587"
  [39]="0.213065"
  [40]="0.230729"
  [41]="0.223826"
  [42]="0.230962"
  [43]="0.259831"
  [44]="0.243105"
  [45]="0.245392"
  [46]="0.250233"
  [47]="0.256094"
  [48]="0.264325"
  [49]="0.305014"
  [50]="0.312"
  [51]="0.306011"
  [52]="0.285667"
  [53]="0.288621"
  [54]="0.293532"
  [55]="0.30118"
  [56]="0.307271"
  [57]="0.311626"
  [58]="0.316694"
  [59]="0.322824"
  [60]="0.330469"
  [61]="0.333964"
  [62]="0.336461"
  [63]="0.341952"
  [64]="0.349693"
)


printf "b_folder\tpercent_range\n" > "$OUT"

find "$ROOT" -mindepth 1 -maxdepth 1 -type d -name 'b_*' | sort -V |
while IFS= read -r bdir; do
  bname="$(basename "$bdir")"

  [[ "$bname" =~ ^b_([0-9]+)$ ]] || continue
  GIB_TOTAL="${BASH_REMATCH[1]}"

  OFFSET="${OFFSETS[$GIB_TOTAL]:-}"
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
    awk -F'\t' -v offset="$OFFSET" -v gib="$GIB_TOTAL" '
      BEGIN {
        target = gib / 32
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

  total_bytes="$(awk -v gib="$GIB_TOTAL" 'BEGIN { printf "%.0f", gib * 1024 * 1024 * 1024 }')"

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