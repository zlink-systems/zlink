# Supervisor note (overrides the brief where they differ)

## 01:05 UTC — 진단에서 멈추지 말고 production 수정까지 하십시오 (PM 지시)
- brief의 "DIAGNOSIS + PROPOSED FIX ONLY / do not change production code"는 취소합니다. 4단계의 수정안을 이 worktree(perf/4353-dotnet-l3-wire)의 production .NET 코드에 구현하십시오.
- 조건: 규칙 수(판정 위치: frame bytes 결정, buffer 소유, 닫기)는 지금 이하, wire bytes 동일(byte-identical frame 시험), 메시지당 할당 계수 시험(수정 전 실패 → 수정 후 통과, 로그 보존). 새 flag·queue·lock·cache·timer 없음. 한 frame에 writer 하나(빠른 경로 + fallback 금지).
- 시험: 바꾼 class의 관련 Framework 시험, `bash scripts/format/format.sh --check dotnet`. Release build rc 0, DLL이 source보다 최신인지 확인.
- 수정 뒤 같은 두 셀(send 1K, backpressure 4K)을 다시 profile해 전후 표를 붙이십시오.
- local commit만(한국어 메시지, 끝에 (#4353)). push·GitHub 금지는 그대로입니다. 시간 상한은 2.5시간으로 늘립니다.
