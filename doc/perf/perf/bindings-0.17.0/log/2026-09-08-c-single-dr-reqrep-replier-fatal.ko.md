# C Single DEALER_ROUTER_REQREP tcp 64 B·256 B 간헐 실패 — 러너 종료·카운터 결함 (2026-09-08)

작업: codex(sol/high) `c-dr-reqrep-fatal`, 감독자 검토 채택. 변경 파일:
`bindings/c/perf/single/common/perf_single_reqrep.hpp`, `bindings/c/perf/single/src/perf_dealer_router_reqrep.cpp`.
Core 소스·측정 조건(timeout·retry·HWM·active 창·admission/completion 모델) 무변경. **Core 결함 아님.**

## 증상

오늘 4회 재현(`sg1` 13:01, `sgfix-rust` 15:38 256 B, `sgfix-node` 16:10, `sg2` 17:22). 종료 시
`replier_fatal=1 received=959982 replied=0 completed=694475` 형태 — requester는 정상(in_flight 0)인데
replier가 fatal이고 `replied`가 0.

## 원인 (진단 보강 뒤 cdrr2·cdrr3 재현으로 확정)

1. **`replied=0`은 카운터 결함.** 기본 2-part reply 경로(`MORE(payload) → FINAL(empty)`)가 성공 뒤 `continue`로
   빠져 `replied` 증가문을 건너뛰었다. reply 자체는 정상 전송됐다(requester completion 69만 건과 양립).
2. **fatal의 실체는 `zlink_reply_part(FINAL) → ZLINK_SUBMIT_BACKPRESSURED(1), errno=EAGAIN(11)`.** requester가
   completion drain을 끝낸 뒤에도 replier는 backlog 앞의 요청에 계속 reply했고, in-band STOP에 도달하기 전에
   reply 쪽이 포화됐다. 종료 국면의 정상 backpressure를 fatal로 분류한 것이 결함.
3. deadline 전에 admission되지 않은 retained request를 drain 중 WRITABLE 뒤 재제출하던 로직은 정책 위반
   (종료 drain은 deadline 전에 admission된 `in_flight`만 대상).

## 수정

- deadline 이후 retained request 재제출 제거(`clear_retained_request`), admitted `in_flight` completion만 bounded drain.
- DR REQREP은 completion drain 뒤 in-band STOP을 보내지 않고 local stop 플래그로 replier를 종료
  (STOP을 폐기될 backlog 뒤에 두지 않는다). 종료 규칙 3개 → 1개.
- replier가 stop 플래그를 본 뒤의 reply backpressure/실패는 정상 종료로 처리.
- 2-part reply 성공도 `replied`에 집계.
- fatal 단계·API rc·errno를 `reply_state_t`에 보존해 shutdown 로그(측정 경로 밖)에 출력.

## 검증 (모두 `core-pinned/0.17.3`, 64 B·256 B × 5-run)

| 태그 | 결과 |
|---|---|
| cdrr2, cdrr3 | 진단만 추가 — 재현(`reply_final rc=1 errno=11`) |
| cdrr4, cdrr5 | 1차 수정 — 실패 |
| cdrr6, cdrr7 | 최종 — 20/20 성공, rc=0 |

정책 unittest 50/50 통과, `git diff --check` 통과. 64 B 양안정(run별 145 K~790 K ops/s)은 남아 있으나 종료 실패는
재발하지 않았다. report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260908_17{3345,3841,4336,4849,5356,5520}_cdrr{2..7}.txt`.
