#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PRESET="${OMM_LATENCY_PRESET:-latency-release}"
MONITORING="${OMM_LATENCY_MONITORING:-deferred}"
PIN_CORES="${OMM_LATENCY_PIN_CORES:-auto}"

case "${MONITORING}" in
  deferred|off) ;;
  *)
    echo "OMM_LATENCY_MONITORING must be deferred or off" >&2
    exit 2
    ;;
esac

if [[ -n "${OMM_LATENCY_TEST_PRESET:-}" ]]; then
  TEST_PRESET="${OMM_LATENCY_TEST_PRESET}"
elif [[ "${MONITORING}" == "off" ]]; then
  TEST_PRESET="${PRESET}-monitoring-off"
else
  TEST_PRESET="${PRESET}"
fi

export OMM_LATENCY_MONITORING="${MONITORING}"
export OMM_LATENCY_PIN_CORES="${PIN_CORES}"

cd "${ROOT_DIR}"
cmake --preset "${PRESET}"
cmake --build --preset "${PRESET}" -j"$(nproc)"
ctest --preset "${TEST_PRESET}" --output-on-failure
