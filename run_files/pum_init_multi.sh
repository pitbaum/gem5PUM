#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# USER CONFIG
# ==============================================================================

GEM5_BIN="./build/RISCV/gem5.fast"

# Parallelism
MAX_JOBS=32

# Toolchain
CC="riscv64-linux-gnu-gcc"
CFLAGS=(-O3 -Wall -march=rv64gc -static)

# Sweep control
START_B=1
END_B="AUTO"   # AUTO = total_banks derived from SRC_C after preprocessing

# Variants:
#   NAME controls bin name prefix (NAME_${b}b) and OUT_BASE directory name.
#   OUTDIR will be: OUT_BASE_${b}b
# NOTE: BIN_DIR is set per-variant to the same experiment family (not "single_const_copy").
VARIANTS=(
  "4rk_x16|src/experiments/final_experiments/setup/small/4ra_x16_small.py|src/experiments/final_experiments/pum/full_compress_multi_const_copy_init_copy/4rk_x16.c|src/experiments/results/pum/full_compress_multi_const_copy_init_copy/4rk_x16"
  "4rk_x8|src/experiments/final_experiments/setup/small/4ra_x8_small.py|src/experiments/final_experiments/pum/full_compress_multi_const_copy_init_copy/4rk_x8.c|src/experiments/results/pum/full_compress_multi_const_copy_init_copy/4rk_x8"
 )

# ==============================================================================
# INTERNALS
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
  local name="$1"
  local gem5_cfg="$2"
  local src_c="$3"
  local out_base="$4"

  local bin_dir
  bin_dir="$(dirname "${out_base}")/bins/${name}"
  mkdir -p "${bin_dir}"

  # Prefer CHANNELS if present; otherwise assume 1 (many of your C files omit it).
  local channels ranks_per_ch banks_per_rank total_banks end_b
  channels="$(pp_get_define "${src_c}" CHANNELS || true)"
  ranks_per_ch="$(pp_get_define "${src_c}" RANKS_PER_CH || true)"
  banks_per_rank="$(pp_get_define "${src_c}" BANKS_PER_RANK || true)"

  [[ -n "${ranks_per_ch}" && -n "${banks_per_rank}" ]] || die \
    "Could not derive RANKS_PER_CH/BANKS_PER_RANK via preprocessing from ${src_c}.
Try: ${CC} -E -dM -DBANKS_USED_COMPRESS=1 ${src_c} | egrep 'CHANNELS|RANKS_PER_CH|BANKS_PER_RANK'"

  if [[ -z "${channels}" ]]; then
    channels=1
  fi

  total_banks=$((channels * ranks_per_ch * banks_per_rank))

  end_b="${END_B}"
  if [[ "${end_b}" == "AUTO" ]]; then
    end_b="${total_banks}"
  fi
  if (( end_b > total_banks )); then
    end_b="${total_banks}"
  fi

  echo "=============================================================================="
  echo "Variant: ${name}"
  echo "  GEM5_CFG:   ${gem5_cfg}"
  echo "  SRC_C:      ${src_c}"
  echo "  OUT_BASE:   ${out_base}"
  echo "  BIN_DIR:    ${bin_dir}"
  echo "  TOTAL_BANKS=${total_banks} (CHANNELS=${channels}, RANKS_PER_CH=${ranks_per_ch}, BANKS_PER_RANK=${banks_per_rank})"
  echo "  Sweep: b=${START_B}..${end_b}"
  echo "=============================================================================="

  for b in $(seq "${START_B}" "${end_b}"); do
    wait_for_slot

    local outdir="${out_base}_${b}b"
    local bin="${bin_dir}/${name}_${b}b"

    mkdir -p "${outdir}"

    echo "==== ${name}: b=${b} ===="
    echo "Compile -> ${bin}"
    "${CC}" "${CFLAGS[@]}" -DBANKS_USED_COMPRESS="${b}" -o "${bin}" "${src_c}"

    echo "Run gem5 -> ${outdir}"
    (
      "${GEM5_BIN}" --outdir="${outdir}" "${gem5_cfg}" \
        --cmd "${bin}" --banks "${b}" \
        > "${outdir}/run.log" 2>&1
    ) &
  done

  wait
  echo "Done variant: ${name}"
}

for entry in "${VARIANTS[@]}"; do
  IFS='|' read -r name gem5_cfg src_c out_base <<< "${entry}"
  run_variant "${name}" "${gem5_cfg}" "${src_c}" "${out_base}"
done

echo "All runs finished."
