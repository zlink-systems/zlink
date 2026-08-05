#!/usr/bin/env bash
# migration-stop-check.sh — Stop hook: 마이그레이션이 끝나지 않았으면 재개 메시지를 보낸다
set -euo pipefail

STATE_FILE="$(git rev-parse --show-toplevel 2>/dev/null)/.claude/migration-state.json"

if [ ! -f "$STATE_FILE" ]; then
  exit 0
fi

current_stage=$(jq -r '.current_stage' "$STATE_FILE" 2>/dev/null || echo "0")
blocker=$(jq -r '.blocker // empty' "$STATE_FILE" 2>/dev/null || echo "")

# 11단계 완료 확인
stage_11_status=$(jq -r '.stages["11"].status' "$STATE_FILE" 2>/dev/null || echo "pending")

if [ "$stage_11_status" = "complete" ]; then
  # 마이그레이션 완료 — 조용히 종료
  exit 0
fi

if [ -n "$blocker" ]; then
  cat <<ENDJSON
{
  "systemMessage": "[migration] stage ${current_stage} blocked: ${blocker}. /supervise-migration 으로 재개하세요."
}
ENDJSON
  exit 0
fi

cat <<ENDJSON
{
  "systemMessage": "[migration] stage ${current_stage}/11 진행 중. /loop 3m /supervise-migration 으로 감독 루프를 재시작하세요."
}
ENDJSON
