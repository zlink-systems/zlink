## 요지
#1710·#2674·#2031 — 연결 준비 전 remote request를 언어마다 logical request admission 한 곳이 판정한다(네 언어 같은 형태, PM 승인 설계 2026-10-30).
- connection attempts snapshot은 socket의 기존 단일 관찰자가 POLLCONNECT 뒤에 읽고, admission owner는 관찰자가 반영한 상태만 읽는다
- 결과는 Submit spec §5(:222-254) 규칙과 기존 failure mapper로 분류, 준비 대기는 원래 request deadline 하나에 포함
- 수정 전 실패 → 수정 후 통과, owner 우회 변형에서 실패 재발, main(dc64205204) 병합 뒤 언어 전체 시험 통과(job 보고서 /mnt/d/t/jobs/fx/cx/i1710/dotnet/s2, /mnt/d/t/jobs/fx/cx/carry032/verify-1710-dotnet)

Refs #1710 #2674 #2031

리뷰: zlink(스펙, 감독자 리뷰 완료) · kairos(성능)

🤖 Generated with [Claude Code](https://claude.com/claude-code)

## #3293 수정 (macOS 연결 거부 시험)
- 원인: `Actual_connection_refusal_ends_request_before_its_deadline`가 bind만 하고 listen하지 않은 소켓으로 거부를 만들었는데, macOS는 그런 포트로 온 SYN을 RST 없이 버려 deadline까지 기다렸습니다(실패 6회 모두 osx-arm64).
- 수정: 포트를 예약한 소켓을 연결 전에 닫아 아무도 없는 포트로 연결합니다. 마지막 단언은 유지하고 강화했습니다(AttemptFailed + Refused + Transport 단계).

### 로컬 .NET 시험 (pre-pr.sh, HEAD be4dc62e0a, main c52ae3e0d7 병합)
| 구성 | Unit | Contract | Stream | 결과 |
|---|---|---|---|---|
| Debug net8.0 | 4138 | 98 | 269 | 통과 |
| Release net10.0 | 4136 | 98 | 269 | 통과 |
- Debug·Release 빌드, format.sh --check dotnet 통과
