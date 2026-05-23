#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# PUM 512 partial compress with init copy
# Sweeps:
#   init_partial_compress/{8,16,32,64,128,256}
# For each size:
#   runs {4rk_x16, 4rk_x8
# For each variant:
#   sweeps b = START_B..TOTAL_BANKS
# ==============================================================================

GEM5_BIN="./build/RISCV/gem5.fast"

# Sweep control
START_B=1
END_B="AUTO"          # AUTO = total banks computed from macros in the C file + variant width

# Parallelism (total concurrent gem5 jobs across everything)
MAX_JOBS=64

# Toolchain
CC="riscv64-linux-gnu-gcc"
CFLAGS=(-O3 -Wall -march=rv64gc -static)

# Which init sizes to run (directory names under init_partial_compress/)
INIT_SIZES=(8 16 32 64 128 256)

# Base directories
SRC_BASE="src/experiments/final_experiments/pum/init_partial_compress"
OUT_BASE="src/experiments/results/pum"

die() { echo "ERROR: $*" >&2; exit 1; }

wait_for_slot() {
  while (( $(jobs -pr | wc -l) >= MAX_JOBS )); do
    wait -n
  done
}

# Extract a numeric macro from the C file:  #define NAME <number>
get_define_num() {
  local file="$1"
  local name="$2"
  awk -v n="$name" '
    $1=="#define" && $2==n {
      x=$3
      gsub(/[()]/,"",x)
      if (x ~ /^[0-9]+$/) { print x; exit }
    }
  ' "$file"
}

banks_per_rank_from_variant_name() {
  local name="$1"
  case "$name" in
    *x8*)  echo 16 ;;
    *x16*)      echo 8 ;;
    *)          return 1 ;;
  esac
}

compute_total_banks() {
  local src_c="$1"
  local exp_name="$2"

  local channels ranks_per_ch banks_per_rank

  channels="$(get_define_num "${src_c}" CHANNELS || true)"
  ranks_per_ch="$(get_define_num "${src_c}" RANKS_PER_CH || true)"

  [[ -n "${ranks_per_ch}" ]] || die "Could not parse RANKS_PER_CH from ${src_c}"

  if [[ -z "${channels}" ]]; then
    channels=1
  fi

  banks_per_rank="$(banks_per_rank_from_variant_name "${exp_name}" || true)"
  [[ -n "${banks_per_rank}" ]] || die "Could not infer BANKS_PER_RANK from exp_name='${exp_name}' (expected x8/x16 in name)"

  echo $((channels * ranks_per_ch * banks_per_rank))
}

run_one_variant() {
  local exp_root="$1"
  local exp_name="$2"
  local gem5_cfg="$3"
  local src_c="$4"

  local out_root="${exp_root}/${exp_name}"
  local bin_root="${exp_root}/bins/${exp_name}"
  mkdir -p "${bin_root}"

  local total_banks end_b
  total_banks="$(compute_total_banks "${src_c}" "${exp_name}")"

  end_b="${END_B}"
  if [[ "${end_b}" == "AUTO" ]]; then
    end_b="${total_banks}"
  fi
  if (( end_b > total_banks )); then
    end_b="${total_banks}"
  fi

  echo "=============================================================================="
  echo "Exp root: ${exp_root}"
  echo "Variant:  ${exp_name}"
  echo "  GEM5_CFG:    ${gem5_cfg}"
  echo "  SRC_C:       ${src_c}"
  echo "  TOTAL_BANKS: ${total_banks}"
  echo "  Sweep:       b=${START_B}..${end_b}"
  echo "  Bins:        ${bin_root}"
  echo "  Out:         ${out_root}_<b>b/"
  echo "=============================================================================="

  for b in $(seq "${START_B}" "${end_b}"); do
    wait_for_slot

    local outdir="${out_root}_${b}b"
    local bin="${bin_root}/${exp_name}_${b}b"
    mkdir -p "${outdir}"

    echo "==== ${exp_name}: b=${b} ===="
    echo "Compile: ${bin}"
    "${CC}" "${CFLAGS[@]}" -DBANKS_USED_COMPRESS="${b}" -o "${bin}" "${src_c}"

    echo "Run: ${outdir}"
    (
      "${GEM5_BIN}" --outdir="${outdir}" "${gem5_cfg}" \
        --cmd "${bin}" --banks "${b}" \
        > "${outdir}/run.log" 2>&1
    ) &
  done

  wait
  echo "Done variant: ${exp_name}"
}

for sz in "${INIT_SIZES[@]}"; do
  exp_root="${OUT_BASE}/partial_initis_${sz}"

  run_one_variant \
    "${exp_root}" \
    "4rk_x16" \
    "src/experiments/final_experiments/setup/small/4ra_x16_small.py" \
    "${SRC_BASE}/${sz}/4rk_x16.c"

  run_one_variant \
    "${exp_root}" \
    "4rk_x8" \
    "src/experiments/final_experiments/setup/small/4ra_x8_small.py" \
    "${SRC_BASE}/${sz}/4rk_x8.c"
done

echo "All runs finished."
