#!/usr/bin/env bash
set -euo pipefail

GEM5_BIN="./build/RISCV/gem5.fast"
GEM5_CFG="src/experiments/final_experiments/setup/read_big/4ra_x16_big.py"

SRC_C="src/experiments/final_experiments/read/error_read/slow_run/4rk_x16.c"
OUT_ROOT="src/experiments/results/read/error_read/slow_run/k_sweepx16/"
BIN="${OUT_ROOT}/bin/test"

CC="riscv64-linux-gnu-gcc"
CFLAGS=(-O3 -Wall -march=rv64gc -static)

M5_INC="$(pwd)/include"
M5_LIB="$(pwd)/util/m5/build/riscv/out"
[[ -f "${M5_LIB}/libm5.a" ]] || { echo "Missing ${M5_LIB}/libm5.a"; exit 1; }

# sweep controls
START_K="${START_K:-1}"
STEP_K="${STEP_K:-8388608}"

MAX_K_LINES=$(( (64<<30) / 64 ))   # 1,073,741,824
END_K="${END_K:-$MAX_K_LINES}"

MAX_JOBS="${MAX_JOBS:-100}"

wait_for_slot() {
  while (( $(jobs -pr | wc -l) >= MAX_JOBS )); do
    wait -n
  done
}

mkdir -p "${OUT_ROOT}/bin"

echo "== Build ${BIN} =="
"${CC}" "${CFLAGS[@]}" -I"${M5_INC}" \
  "${SRC_C}" -L"${M5_LIB}" -lm5 \
  -o "${BIN}"

echo "== Sweep k=${START_K}..${END_K} step ${STEP_K} =="
for ((k=START_K; k<=END_K; k+=STEP_K)); do
  wait_for_slot
  outdir="${OUT_ROOT}/k_${k}"
  mkdir -p "${outdir}"
  (
    "${GEM5_BIN}" --outdir="${outdir}" "${GEM5_CFG}" \
      --cmd "${BIN}" --options "${k}" \
      > "${outdir}/run.log" 2>&1
  ) &
  echo "launched k=${k} -> ${outdir}"
done

wait
echo "Done."