#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# Only PUM 512 compress with init copy
# ==============================================================================

GEM5_BIN="./build/RISCV/gem5.fast"

# --- experiment naming / outputs ---
EXP_ROOT="src/experiments/results/pum/full_compress_init_copy"

# --- sweep control ---
START_B=1
END_B="AUTO"   # AUTO = total_banks derived from SRC_C after preprocessing

# --- parallelism ---
MAX_JOBS=64

# --- toolchain ---
CC="riscv64-linux-gnu-gcc"
CFLAGS=(-O3 -Wall -march=rv64gc -static)

# Variants: "EXP_NAME|GEM5_CFG|SRC_C"
VARIANTS=(
  "4rk_x16|src/experiments/final_experiments/setup/small/4ra_x16_small.py|src/experiments/final_experiments/pum/full_compress_init_copy/4rk_x16.c"
  "4rk_x8|src/experiments/final_experiments/setup/small/4ra_x8_small.py|src/experiments/final_experiments/pum/full_compress_init_copy/4rk_x8.c"
)

# ==============================================================================
# INTERNALS (no edits usually needed)
# ==============================================================================

die() { echo "ERROR: $*" >&2; exit 1; }

wait_for_slot() {
  while (( $(jobs -pr | wc -l) >= MAX_JOBS )); do
    wait -n
  done
}

# Preprocess-aware macro extraction (handles conditional BANKS_PER_RANK)
pp_get_define() {
  local file="$1"
  local name="$2"
  "${CC}" -E -dM -DBANKS_USED_COMPRESS=1 "${file}" 2>/dev/null \
    | awk -v n="${name}" '$1=="#define" && $2==n {print $3; exit}'
}

run_variant() {
  local exp_name="$1"
  local gem5_cfg="$2"
  local src_c="$3"

  local out_root="${EXP_ROOT}/${exp_name}"
  local bin_root="${EXP_ROOT}/bins/${exp_name}"
  mkdir -p "${bin_root}"

  local ranks_per_ch banks_per_rank total_banks end_b
  ranks_per_ch="$(pp_get_define "${src_c}" RANKS_PER_CH || true)"
  banks_per_rank="$(pp_get_define "${src_c}" BANKS_PER_RANK || true)"

  [[ -n "${ranks_per_ch}" && -n "${banks_per_rank}" ]] || die \
    "Could not derive RANKS_PER_CH/BANKS_PER_RANK via preprocessing from ${src_c}.
Try: ${CC} -E -dM -DBANKS_USED_COMPRESS=1 ${src_c} | egrep 'RANKS_PER_CH|BANKS_PER_RANK'"

  total_banks=$((ranks_per_ch * banks_per_rank))

  end_b="${END_B}"
  if [[ "${end_b}" == "AUTO" ]]; then
    end_b="${total_banks}"
  fi
  if (( end_b > total_banks )); then
    end_b="${total_banks}"
  fi

  echo "=============================================================================="
  echo "Variant: ${exp_name}"
  echo "  GEM5_CFG: ${gem5_cfg}"
  echo "  SRC_C:    ${src_c}"
  echo "  TOTAL_BANKS=${total_banks} (RANKS_PER_CH=${ranks_per_ch}, BANKS_PER_RANK=${banks_per_rank})"
  echo "  Sweep: b=${START_B}..${end_b}"
  echo "  Bins:  ${bin_root}"
  echo "  Out:   ${out_root}_<b>b/"
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

for entry in "${VARIANTS[@]}"; do
  IFS='|' read -r exp_name gem5_cfg src_c <<< "${entry}"
  run_variant "${exp_name}" "${gem5_cfg}" "${src_c}"
done

echo "All runs finished."
