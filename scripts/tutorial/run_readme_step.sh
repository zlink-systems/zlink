#!/usr/bin/env bash
# README의 빌드·실행·검증 절에서 뽑은 명령을 돌리고, 아직 그 절이 #655 규칙(fenced
# block에 title="linux"/"windows")을 따르지 않으면 선택한 fallback 명령으로 대신한다.
# .github/workflows/standalone-zips.yml이 checkout 없는 job에서 쓴다.
#
# 사용: run_readme_step.sh <README 경로> <절 이름> <linux|windows> -- [fallback 명령...]
set -euo pipefail

readme="$1"; section="$2"; platform="$3"; shift 3
[ "${1:-}" = "--" ] || { echo "usage: $0 <README> <section> <platform> -- [fallback...]" >&2; exit 2; }
shift

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
err_file="$(mktemp)"
trap 'rm -f "${err_file}"' EXIT

if block="$(python3 "${script_dir}/extract_readme_step.py" "${readme}" "${section}" "${platform}" 2>"${err_file}")"; then
  echo "README extraction: running the '${section}' (${platform}) block from ${readme} verbatim"
  if [ "${platform}" = "windows" ]; then
    # A "windows"-titled block is PowerShell (the README convention pairs
    # platform=windows with a ```powershell fence); linux blocks are bash.
    pwsh -NoProfile -Command "${block}"
  else
    bash -c "${block}"
  fi
else
  if [ "$#" -eq 0 ]; then
    echo "README extraction failed: $(cat "${err_file}")" >&2
    exit 1
  fi
  echo "README extraction skipped: $(cat "${err_file}")"
  echo "falling back to this workflow's own '${section}' command"
  "$@"
fi
