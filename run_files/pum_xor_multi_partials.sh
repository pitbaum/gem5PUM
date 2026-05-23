#!/usr/bin/env bash
set -euo pipefail

GEM5_ROOT="${GEM5_ROOT:-$PWD}"          # set explicitly if script is not run from gem5 root
GEM5_BIN="${GEM5_ROOT}/build/RISCV/gem5.fast"

START_B=1
END_B="AUTO"
MAX_JOBS=64

CC="riscv64-linux-gnu-gcc"

# gem5 headers
GEM5_INC="-I${GEM5_ROOT}/include"

# m5 library paths (common locations; use the one that exists)
M5_LIB=""
if [[ -f "${GEM5_ROOT}/util/m5/build/riscv/out/libm5.a" ]]; then
  M5_LIB="${GEM5_ROOT}/util/m5/build/riscv/out/libm5.a"
elif [[ -f "${GEM5_ROOT}/util/m5/build/riscv64/out/libm5.a" ]]; then
  M5_LIB="${GEM5_ROOT}/util/m5/build/riscv64/out/libm5.a"
fi

CFLAGS=(-O3 -Wall -march=rv64gc -static "${GEM5_INC}")
LDFLAGS=()
[[ -n "${M5_LIB}" ]] && LDFLAGS+=("${M5_LIB}")

INIT_SIZES=(8 16 32 64 128 256 512)

SRC_BASE="src/experiments/final_experiments/pum/xor/multi"
OUT_BASE="src/experiments/results/pum/xor_multi_partial"

die() { echo "ERROR: $*" >&2; exit 1; }

wait_for_slot() {
  while (( $(jobs -pr | wc -l) >= MAX_JOBS )); do
    wait -n
  done
}

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
    *x16*) echo 8 ;;
    *)     return 1 ;;
  esac
}

compute_total_banks() {
  local src_c="$1"
  local exp_name="$2"

  local channels ranks_per_ch banks_per_rank

  channels="$(get_define_num "${src_c}" CHANNELS || true)"
  ranks_per_ch="$(get_define_num "${src_c}" RANKS_PER_CH || true)"

  [[ -n "${ranks_per_ch}" ]] || die "Could not parse RANKS_PER_CH from ${src_c}"
  [[ -n "${channels}" ]] || channels=1

  banks_per_rank="$(banks_per_rank_from_variant_name "${exp_name}" || true)"
  [[ -n "${banks_per_rank}" ]] || die "Could not infer BANKS_PER_RANK from exp_name='${exp_name}'"

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
  [[ "${end_b}" == "AUTO" ]] && end_b="${total_banks}"
  (( end_b > total_banks )) && end_b="${total_banks}"

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
    "${CC}" "${CFLAGS[@]}" -DBANKS_USED_COMPRESS="${b}" \
      -o "${bin}" "${src_c}" "${LDFLAGS[@]}"

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
  exp_root="${OUT_BASE}/xor_partial_initis_${sz}"

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