#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"                     # top-level folder to scan
OUT="${2:-simseconds_summary.txt}" # output file

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

# Collect: <subfolder_name>\t<simSeconds>
find "$ROOT" -type f -name "stats.txt" -print0 |
while IFS= read -r -d '' f; do
  folder="$(basename "$(dirname "$f")")"
  simSeconds="$(awk '$1=="simSeconds"{print $2; exit}' "$f" || true)"
  [[ -n "${simSeconds:-}" ]] || simSeconds="NA"
  printf "%s\t%s\n" "$folder" "$simSeconds" >> "$tmp"
done

# Sort by: *_x<GroupId>_<Order>b  (GroupId asc, then Order asc)
# Output:
# x8
# 0.034194
# 0.059323
# ...
# x16
# 0.068142
# ...
{
  while IFS=$'\t' read -r folder sim; do
    gid=999999999
    ord=999999999
    xlabel="other"

    if [[ "$folder" =~ _x([0-9]+)_([0-9]+)b$ ]]; then
      gid="${BASH_REMATCH[1]}"
      ord="${BASH_REMATCH[2]}"
      xlabel="x${BASH_REMATCH[1]}"
    fi

    printf "%09d\t%09d\t%s\t%s\n" "$gid" "$ord" "$xlabel" "$sim"
  done < "$tmp" |
  sort -t $'\t' -k1,1n -k2,2n |
  awk -F'\t' '
    BEGIN { prev = "" }
    {
      if ($3 != prev) {
        if (prev != "") print ""
        print $3
        prev = $3
      }
      print $4
    }
  '
} > "$OUT"