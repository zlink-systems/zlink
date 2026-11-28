#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

pass=0
fail=0

for sample in samples/*_sample; do
  echo "==> ${sample}"
  if go run "./${sample}"; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
  fi
done

echo "samples: pass=${pass} fail=${fail}"
if [[ "${fail}" -ne 0 ]]; then
  exit 1
fi
