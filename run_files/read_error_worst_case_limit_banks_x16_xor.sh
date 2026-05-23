#!/usr/bin/env bash
set -euo pipefail

GEM5_BIN="./build/RISCV/gem5.fast"
GEM5_CFG="src/experiments/final_experiments/setup/read_big/4ra_x16_big.py"

SRC_C="src/experiments/final_experiments/read/error_read/slow_run_limit_banks_xor/4rk_x16.c"
OUT_ROOT="src/experiments/results/read/error_read/slow_run_limit_banks_xor/x16/"
BIN="${OUT_ROOT}/bin/test"

CC="riscv64-linux-gnu-gcc"
CFLAGS=(-O3 -Wall -march=rv64gc -static)

M5_INC="$(pwd)/include"
M5_LIB="$(pwd)/util/m5/build/riscv/out"
[[ -f "${M5_LIB}/libm5.a" ]] || { echo "Missing ${M5_LIB}/libm5.a"; exit 1; }

# Concurrency
MAX_JOBS="${MAX_JOBS:-128}"

# Sweep banks (x16 has TOTAL_BANKS = 4 ranks * 8 banks/rank = 32)
START_B="${START_B:-1}"
END_B="${END_B:-32}"

# About how many k-points per bank sweep
NUM_STEPS="${NUM_STEPS:-50}"

# k range per bank:
# Effective usable capacity per bank is now 1 GiB, so max unique cachelines for N banks is:
#   max_k_lines(N) = N * (1GiB / 64B)
K_PER_BANK=$(( (1<<30) / 64 ))   # 16,777,216 cachelines per 1 GiB effective bank capacity

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
  END_K=$(( b * K_PER_BANK ))

  span=$(( END_K - START_K ))
  if (( span < 0 )); then
    echo "Skipping b=${b}: END_K < START_K"
    continue
  fi

  if (( NUM_STEPS <= 1 )); then
    step=$(( END_K - START_K ))
    if (( step < 1 )); then step=1; fi
  else
    step=$(( span / (NUM_STEPS - 1) ))
    if (( step < 1 )); then step=1; fi
  fi

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