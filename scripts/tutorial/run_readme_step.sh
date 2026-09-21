#!/usr/bin/env bash
# README의 내려받기와 설치·빌드·실행·검증 절에서 뽑은 명령을 돌린다. 명령은 README가
# 정의한 그대로여야 하며, 문서에 해당 platform block이 없으면 즉시 실패한다.
# .github/workflows/examples-smoke.yml이 checkout 없는 job에서 쓴다.
#
# 사용: run_readme_step.sh <README 경로> <절 이름> <linux|windows>
set -euo pipefail

if [ "$#" -ne 3 ]; then
  echo "usage: $0 <README> <section> <linux|windows>" >&2
  exit 2
fi
readme="$1"; section="$2"; platform="$3"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
err_file="$(mktemp)"
trap 'rm -f "${err_file}"' EXIT

block="$(python3 "${script_dir}/extract_readme_step.py" "${readme}" "${section}" "${platform}" 2>"${err_file}")" && status=0 || status=$?
if [ "${status}" -eq 3 ]; then
  echo "README extraction: $(cat "${err_file}")"
  exit 0
fi
if [ "${status}" -ne 0 ]; then
  echo "README extraction failed: $(cat "${err_file}")" >&2
  exit 1
fi

echo "README extraction: running the '${section}' (${platform}) block from ${readme} verbatim"
if [ "${platform}" = "windows" ]; then
  # A "windows"-titled block is PowerShell (the README convention pairs
  # platform=windows with a ```powershell fence); linux blocks are bash.
  pwsh -NoProfile -Command "${block}"
else
  # A README run block may leave server/client processes in the background for
  # the following verify step. Give the block its own process group and retain
  # that group id; the workflow's cleanup step can then terminate exactly what
  # this job's README block started without matching process names.
  pid_file=".readme-step.pgids"
  # The child records its own group id before it runs the block. Reading the
  # group from outside right after the fork is a race: until setsid(2) has
  # taken effect ps still reports this script's group, and retaining that
  # would make the workflow's cleanup step kill the runner itself (exit 143).
  setsid bash -c 'ps -o pgid= -p "$$" | tr -d " " >> "$1"; shift; exec bash -c "$1"' \
    _ "${pid_file}" "${block}" &
  block_pid=$!
  wait "${block_pid}"
fi
