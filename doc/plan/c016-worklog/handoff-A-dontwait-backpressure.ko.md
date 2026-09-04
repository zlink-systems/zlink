# A 인계 — DONTWAIT send 계약 변경 (0.17.0, 판정 D-B79)

작성: 머신 B, 2026-09-04 15:10. 상태: Core 수정 job 진행 중(main 트리, 커밋 전). 완료 시 이 문서에 커밋 해시와 영향 목록을 추가한다.

## 1. 바뀌는 계약 (사용자 확정)

| 상황 | 0.16.0 현행 (bb66e85376) | 0.17.0 (신규) |
|---|---|---|
| `zlink_send`/`zlink_send_part` DONTWAIT FINAL, 즉시 admission 가능 | `ZLINK_SUBMIT_OK`, ID 0, completion 없음 | 동일 |
| DONTWAIT FINAL, pipe HWM/byte credit 부족 | pending 수락: nonzero ID, 나중에 SEND completion, pending pool 기본 무제한 | **`ZLINK_SUBMIT_BACKPRESSURED`, `errno == EAGAIN`, ID 0, completion 없음, payload는 호출자 소유** |
| DONTWAIT FINAL, endpoint 미준비(handshake/재연결) | pending 수락 | 즉시 거절(기존 errno 규칙: `EAGAIN`/`ENOTCONN`/`EHOSTUNREACH`), 보관 없음 |
| "다시 보내도 됨" 신호 | (pending이 알아서 전송) | poller `ZLINK_POLLOUT`(level-trigger, 유실 없음) |
| completion queue 용도 | SEND pending 완료 + REQUEST 결과 | **REQUEST reply/timeout 등 접수된 작업 결과만** (일반 SEND는 completion 없음) |
| `ZLINK_OPT_PENDING_MAX_MSGS/BYTES` | SEND·REQUEST 공유 상한(기본 0 = 무제한) | SEND에는 no-op(REQUEST 현행 유지). enum 값·ABI 유지 |
| blocking `NONE` send | 호출 스레드 park, `SNDTIMEO` 스냅샷 | 동일 |
| `zlink_request_part` (REQREP) | pending/timeout/reply completion | **이번 변경 범위 밖, 현행 유지** |

핵심: HWM은 다시 흐름 제어 신호다. Core는 패킷을 두 번째 큐에 보관하지 않는다. 비동기(코루틴) 모델은
**DONTWAIT → EAGAIN → 그 소켓의 POLLOUT 대기 → 깨어나면 같은 패킷 재전송**이다. 응용의 재전송 스레드/루프를
없애는 것은 "POLLOUT을 어떤 컨텍스트에 전달하느냐"로 해결하고, 패킷 보관으로 해결하지 않는다.

## 2. framework가 바꿔야 할 것

- send 경로에서 "nonzero completion ID를 받아 completion을 기다린다"는 가정을 제거한다. DONTWAIT가
  `BACKPRESSURED`/`EAGAIN`이면 그 코루틴/작업을 해당 소켓의 writable 대기 목록에 넣고, framework의 poller 루프가
  `POLLOUT`을 받으면 깨워서 **같은 패킷을 다시 제출**한다. 별도 OS 스레드·timer·sleep 재시도는 만들지 않는다
  (perf 정책 §1.2의 C reference 모델과 동일: `send_pending` 플래그 + POLLOUT 재전송).
- 같은 소켓의 후속 submit 순서는 framework(언어 runtime)가 관리한다(먼저 막힌 패킷이 먼저 나가야 하면 대기열은
  framework 측 논리 큐다 — 이는 응용 데이터 순서 책임이며 Core 큐가 아니다).
- `ZLINK_OPT_PENDING_MAX_*`를 SEND 목적으로 설정하는 코드는 제거(REQUEST용은 유지).
- REQUEST(요청/응답) 경로는 그대로.
- 현재 EAGAIN/POLLOUT/BACKPRESSURED를 다루는 코드가 이미 있는 언어: cpp 6파일, dotnet 32, java 15. **node는 0건**
  (재전송 경로 신규 필요). 정확한 파일:행 영향 목록은 Core job 요약
  `doc/plan/c016-worklog/dontwait-backpressure-core-summary.md`에 실린다(완료 시).

## 3. 검증 방법(framework 쪽)

- 테스트 시나리오: HWM까지 채움 → DONTWAIT가 BACKPRESSURED → peer drain → POLLOUT → 재전송 성공, 데이터 순서 유지.
  sleep 기반 대기 금지, 5회 반복 green.
- Core 회귀 테스트(B가 추가): `core/tests/integration/test_wake_invariants.cpp`의 HWM EAGAIN→POLLOUT 케이스와
  100 client × 64 KiB 케이스, 신규 DONTWAIT 거절 케이스.

## 4. 버전·문서

- Core·bindings 버전 0.17.0(사용자 결정). B가 Core 커밋 뒤 `VERSION`/`BINDINGS_VERSION` 범프 + `sync-version.py`
  + 수동 매니페스트(python/rust/java/node/go allowlist)까지 처리.
- 스펙 정정(B, 별도 커밋, ko/en): `core/doc/spec/core/socket/README.ko.md` 116·375~389·923~955행, `06-dealer` 100·289~290·323~327행,
  기타 socket 문서 DONTWAIT 문장, `05-polling`에 POLLOUT 관측 명시, `doc/perf/PERF_MULTI_TEST_POLICY.md §1.2` pull 대응 문단.
- bindings(c/cpp/go/rust/python/node/java/dotnet) API 정합은 B가 언어별 job으로 처리(배포 제외).

## 5. 진행 상태

- [ ] Core 수정 (job c016-dontwait-bp, sol ultra+fast) — 진행 중: DONTWAIT를 단일 admission 시도로 분리, pending/completion reservation 경로 제거(3파일 +30/−147 시점)
- [ ] B gate (dev ctest, release-gate, wake-invariant, hotpath_gate, 바인딩 테스트) + 벤치 before/after 표
- [ ] 0.17.0 범프 커밋
- [ ] 스펙 정정 커밋(diff 사용자 확인 후)
- [ ] bindings 언어별 job
