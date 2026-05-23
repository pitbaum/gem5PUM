#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# Sweep runner (folder -> variant -> banks)
#
# Like your init_partial_compress/{8,16,...} script, but for the LUT read runs
# using:
#   - C + .S split compile (so m5ops header isn't injected into .S)
#   - static link against libm5.a (m5_reset_stats/m5_dump_stats)
#   - sweep b=START_B..END_B (AUTO = derived from macros)
# ==============================================================================

GEM5_BIN="./build/RISCV/gem5.fast"
MAX_JOBS=64

# Sweep control
START_B=1
END_B="AUTO"

# Toolchain
CC="riscv64-linux-gnu-gcc"

# Compile flags (shared)
COMMON_CFLAGS=(-O3 -Wall -march=rv64gc_zicbom)
COMMON_LDFLAGS=(-static)

# Always-linked ASM sources (compiled once per variant folder)
ASM_SRCS=(cbo.S)

# ------------------------------------------------------------------------------
# Folder sweep (edit these)
# Each folder should contain 4rk_x16.c and 4rk_x8.c (or adjust below).
# ------------------------------------------------------------------------------
SRC_BASE="src/experiments/final_experiments/read/lut_pum_read/xor/init"
OUT_BASE="src/experiments/results/read/xor/xor_init"

# Example: run for multiple source folders under SRC_BASE/
# e.g. SRC_BASE/full_size, SRC_BASE/touch_only, SRC_BASE/cache_touch_only, ...
FOLDERS=(
  "8"
  "16"
  "32"
  "64"
  "128"
  "256"
  "512"
)

# gem5 configs per variant (edit if needed)
CFG_X16="src/experiments/final_experiments/setup/big/4ra_x16_big.py"
CFG_X8="src/experiments/final_experiments/setup/big/4ra_x8_big.py"

# ------------------------------------------------------------------------------
# M5OPS (libm5.a)
# ------------------------------------------------------------------------------
GEM5_ROOT="$(pwd)"
M5_INC="${GEM5_ROOT}/include"
M5_LIB="${GEM5_ROOT}/util/m5/build/riscv/out"
M5_LIBFILE="${M5_LIB}/libm5.a"

# Force-include m5ops header ONLY for C compilation (NOT for .S)
M5_C_CPPFLAGS=(-I"${M5_INC}" -include gem5/m5ops.h)

# Link against libm5.a
M5_LDFLAGS=(-L"${M5_LIB}" -lm5)

# If -static and libm5 pulls in C++ bits, only add -lstdc++ if static archive exists.
LIBSTDCXX_A="$("${CC}" -print-file-name=libstdc++.a 2>/dev/null || true)"
if [[ -n "${LIBSTDCXX_A}" && "${LIBSTDCXX_A}" != "libstdc++.a" && -f "${LIBSTDCXX_A}" ]]; then
  M5_LDFLAGS+=(-lstdc++)
fi

# ==============================================================================
# INTERNALS
# ==============================================================================
die() { echo "ERROR: $*" >&2; exit 1; }

wait_for_slot() {
  while (( $(jobs -pr | wc -l) >= MAX_JOBS )); do
    wait -n
  done
}

# Extract numeric macro:  #define NAME <number>
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

banks_per_rank_from_device_x() {
  local dev="$1"
  case "$dev" in
    8)   echo 16 ;;  # x8 -> 16 banks/rank
    16)  echo 8  ;;  # x16 -> 8 banks/rank
    *)   return 1 ;;
  esac
}

preflight() {
  [[ -x "${GEM5_BIN}" ]] || die "gem5 binary not found: ${GEM5_BIN}"
  [[ -f "${M5_LIBFILE}" ]] || die "libm5.a not found: ${M5_LIBFILE} (build util/m5 first)"
  for s in "${ASM_SRCS[@]}"; do
    [[ -f "${s}" ]] || die "Missing ASM source: ${s}"
  done
  for f in "${FOLDERS[@]}"; do
    [[ -d "${SRC_BASE}/${f}" ]] || die "Missing folder: ${SRC_BASE}/${f}"
  done
}

# Compile ASM once for a folder+variant and reuse in all b sweeps
compile_asm_objects_once() {
  local objdir="$1"
  mkdir -p "${objdir}"

  local objs=()
  for s in "${ASM_SRCS[@]}"; do
    local o="${objdir}/$(basename "${s}").o"
    if [[ ! -f "${o}" ]]; then
      "${CC}" "${COMMON_CFLAGS[@]}" -c "${s}" -o "${o}"
    fi
    objs+=("${o}")
  done

  # print objs separated by spaces
  echo "${objs[*]}"
}

compile_and_link() {
  local bin="$1"
  local src_c="$2"
  local b="$3"
  local asm_objs_str="$4"

  local obj_c="${bin}.o"
  local asm_objs=()
  # shellcheck disable=SC2206
  asm_objs=(${asm_objs_str})

  # Compile C (force-include m5ops header here)
  "${CC}" "${COMMON_CFLAGS[@]}" "${M5_C_CPPFLAGS[@]}" \
    -DBANKS_USED_COMPRESS="${b}" \
    -c "${src_c}" -o "${obj_c}"

  # Link
  "${CC}" "${COMMON_LDFLAGS[@]}" -o "${bin}" \
    "${obj_c}" "${asm_objs[@]}" \
    "${M5_LDFLAGS[@]}"

  rm -f "${obj_c}"
}

run_one_variant() {
  local exp_root="$1"
  local exp_name="$2"
  local gem5_cfg="$3"
  local src_c="$4"

  local out_root="${exp_root}/${exp_name}"
  local bin_root="${exp_root}/bins/${exp_name}"
  local asm_obj_dir="${bin_root}/asm"
  mkdir -p "${bin_root}"

  local channels ranks_per_ch device_x_bits banks_per_rank total_banks end_b

  channels="$(get_define_num "${src_c}" CHANNELS || true)"
  ranks_per_ch="$(get_define_num "${src_c}" RANKS_PER_CH || true)"
  device_x_bits="$(get_define_num "${src_c}" DEVICE_X_BITS || true)"

  [[ -n "${channels}" ]] || channels=1
  [[ -n "${ranks_per_ch}" ]] || die "Could not parse RANKS_PER_CH from ${src_c}"
  [[ -n "${device_x_bits}" ]] || die "Could not parse DEVICE_X_BITS from ${src_c}"

  banks_per_rank="$(banks_per_rank_from_device_x "${device_x_bits}" || true)"
  [[ -n "${banks_per_rank}" ]] || die "Unsupported DEVICE_X_BITS=${device_x_bits} in ${src_c} (expected 8 or 16)"

  total_banks=$((channels * ranks_per_ch * banks_per_rank))

  end_b="${END_B}"
  if [[ "${end_b}" == "AUTO" ]]; then
    end_b="${total_banks}"
  fi
  if (( end_b > total_banks )); then
    end_b="${total_banks}"
  fi

  local asm_objs_str
  asm_objs_str="$(compile_asm_objects_once "${asm_obj_dir}")"

  echo "=============================================================================="
  echo "Exp root: ${exp_root}"
  echo "Variant:  ${exp_name}"
  echo "  GEM5_CFG:   ${gem5_cfg}"
  echo "  SRC_C:      ${src_c}"
  echo "  CHANNELS=${channels} RANKS_PER_CH=${ranks_per_ch} DEVICE_X_BITS=${device_x_bits} BANKS_PER_RANK=${banks_per_rank}"
  echo "  TOTAL_BANKS=${total_banks}"
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
    echo "Compile+Link: ${bin}"
    compile_and_link "${bin}" "${src_c}" "${b}" "${asm_objs_str}"

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

# ==============================================================================
# MAIN
# ==============================================================================
preflight

for folder in "${FOLDERS[@]}"; do
  exp_root="${OUT_BASE}/${folder}"

  # x16
  run_one_variant \
    "${exp_root}" \
    "4rk_x16" \
    "${CFG_X16}" \
    "${SRC_BASE}/${folder}/4rk_x16.c"

  # x8
  run_one_variant \
    "${exp_root}" \
    "4rk_x8" \
    "${CFG_X8}" \
    "${SRC_BASE}/${folder}/4rk_x8.c"
done

echo "All runs finished."