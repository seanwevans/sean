#!/usr/bin/env bash
set -euo pipefail

BENCH="${BENCH:-./bench}"
OUT="${OUT:-bench_results_$(date +%Y%m%d_%H%M%S).txt}"

producers=(4 8)
consumers=(4 8)
items=(10000000 20000000)
capacities=(1024 65536 1048576)
runs=21

{
  echo "# benchmark matrix"
  echo "# date: $(date -Is)"
  echo "# bench: ${BENCH}"
  echo "# runs: ${runs}"
  echo
} | tee "${OUT}"

for p in "${producers[@]}"; do
  for c in "${consumers[@]}"; do
    for n in "${items[@]}"; do
      for cap in "${capacities[@]}"; do
        args=(--producers "$p" --consumers "$c" --items "$n" --capacity "$cap" --runs "$runs")

        echo "==================================================" | tee -a "${OUT}"
        printf '%q ' "${BENCH}" "${args[@]}" | tee -a "${OUT}"
        echo | tee -a "${OUT}"

        "${BENCH}" "${args[@]}" | tee -a "${OUT}"
        echo | tee -a "${OUT}"
      done
    done
  done
done

echo "wrote ${OUT}"