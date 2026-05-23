#!/usr/bin/env bash
set -euo pipefail

GEM5_BIN="./build/RISCV/gem5.fast"
GEM5_CFG="src/experiments/final_experiments/setup/read_big/4ra_x8_big.py"

SRC_C="src/experiments/final_experiments/read/error_read/slow_run_limit_banks_xor/4rk_x8.c"
OUT_ROOT="src/experiments/results/read/error_read/slow_run_limit_banks_xor/x8/"
BIN="${OUT_ROOT}/bin/test"

CC="riscv64-linux-gnu-gcc"
CFLAGS=(-O3 -Wall -march=rv64gc -static)

M5_INC="$(pwd)/include"
M5_LIB="$(pwd)/util/m5/build/riscv/out"
[[ -f "${M5_LIB}/libm5.a" ]] || { echo "Missing ${M5_LIB}/libm5.a"; exit 1; }

# Concurrency
MAX_JOBS="${MAX_JOBS:-64}"

# Sweep banks
START_B="${START_B:-1}"
END_B="${END_B:-63}"

# About how many k-points per bank sweep
NUM_STEPS="${NUM_STEPS:-50}"

# k range per bank:
# Effective capacity per bank is now 512 MiB, so max unique cachelines touched for N banks is:
#   max_k_lines(N) = N * (512MiB / 64B) = N * 8,388,608
K_PER_BANK=$(( (1<<29) / 64 ))   # 8,388,608 cachelines per 512 MiB effective bank capacity

# Optional: start k at 1 by default
START_K="${START_K:-1}"

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

echo "== Sweep banks=${START_B}..${END_B}, ~${NUM_STEPS} k-points each =="

for ((b=START_B; b<=END_B; b++)); do
  # Max k for this bank count
  END_K=$(( b * K_PER_BANK ))

  # Compute step so we get ~NUM_STEPS points between START_K and END_K (inclusive).
  # Ensure step >= 1.
  span=$(( END_K - START_K ))
  if (( span < 0 )); then
    echo "Skipping b=${b}: END_K < START_K"
    continue
  fi

  step=$(( span / (NUM_STEPS - 1) ))
  if (( step < 1 )); then step=1; fi

  exp_root="${OUT_ROOT}/b_${b}"
  mkdir -p "${exp_root}"

  echo "------------------------------------------------------------------------------"
  echo "Banks active: ${b}"
  echo "  END_K=${END_K}"
  echo "  STEP_K≈${step}  (NUM_STEPS target: ${NUM_STEPS})"
  echo "  Out: ${exp_root}/k_<k>/"
  echo "------------------------------------------------------------------------------"

  for ((k=START_K; k<=END_K; k+=step)); do
    wait_for_slot
    outdir="${exp_root}/k_${k}"
    mkdir -p "${outdir}"
    (
      "${GEM5_BIN}" --outdir="${outdir}" "${GEM5_CFG}" \
        --cmd "${BIN}" --options "${k} ${b}" \
        > "${outdir}/run.log" 2>&1
    ) &
    echo "launched b=${b} k=${k} -> ${outdir}"
  done

  # Ensure we run the exact END_K once (if the step doesn't land on it)
  if (( (END_K - START_K) % step != 0 )); then
    wait_for_slot
    k="${END_K}"
    outdir="${exp_root}/k_${k}"
    mkdir -p "${outdir}"
    (
      "${GEM5_BIN}" --outdir="${outdir}" "${GEM5_CFG}" \
        --cmd "${BIN}" --options "${k} ${b}" \
        > "${outdir}/run.log" 2>&1
    ) &
    echo "launched b=${b} k=${k} (final) -> ${outdir}"
  fi
done

wait
echo "Done."