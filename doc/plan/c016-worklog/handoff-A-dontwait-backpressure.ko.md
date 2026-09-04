# A 인계 — DONTWAIT send 계약 변경 (0.17.0, 판정 D-B79)

작성: 머신 B, 2026-09-04 15:10 / **개정 16:00 (계약 B로 확정)**. 상태: Core 수정 job 진행 중(main 트리, 커밋 전). 완료 시 커밋 해시와 영향 목록을 추가한다.

## 1. 바뀌는 계약 (사용자 확정, B안)

| 상황 | 0.16.0 현행 (bb66e85376) | 0.17.0 (신규, B) |
|---|---|---|
| DONTWAIT FINAL, 즉시 admission 가능 | `ZLINK_SUBMIT_OK`, ID 0, completion 없음 | 동일 |
| DONTWAIT FINAL, 대상 pipe HWM/byte credit 부족 또는 대상 미준비 | pending 수락(payload 보관, 기본 무제한), 나중에 SEND completion | **`ZLINK_SUBMIT_BACKPRESSURED`, `errno == EAGAIN`, completion ID = nonzero 대기 토큰, `user_context` 기억. payload는 호출자 소유(Core는 토큰·대상 pipe·context만 보관)** |
| "다시 보내도 됨" 신호 | (pending이 알아서 전송) | 대상 pipe에 credit이 돌아오면 Core가 completion queue에 **`WRITABLE` completion**(토큰·context·가능하면 RID)을 넣고 poller를 **`POLLOUT`**으로 깨움(POLLCOMPLETION도 level 성립) |
| 앱 동작 | completion을 기다림 | POLLOUT → `zlink_completion_recv`를 NO_DATA까지 pull → WRITABLE의 context로 **정확히 그 코루틴**을 재개 → 같은 패킷 재전송 |
| 대상 단위 | — | PAIR=단일 pipe, DEALER=후보 집합(어느 후보든 열리면 WRITABLE 1건, 재전송 시 Core가 열린 peer 선택), ROUTER/STREAM=지정 RID pipe |
| 토큰 수명 | — | WRITABLE 전달 또는 socket close(잔여 토큰은 terminal completion으로 정리) |
| completion kind | REQUEST, SEND | REQUEST, **WRITABLE**(신규 enum 값, 기존 값 불변); 일반 SEND 성공에는 completion 없음 |
| `ZLINK_OPT_PENDING_MAX_MSGS/BYTES` | SEND·REQUEST 공유(기본 무제한) | SEND에는 no-op(REQUEST 현행). enum 값·ABI 유지 |
| blocking `NONE` send / `zlink_request_part` | 현행 | 현행 유지 |

핵심: HWM은 다시 흐름 제어 신호다. Core는 패킷을 보관하지 않고 "누구를 깨울지"만 정확히 알려준다(대상 단위). 콜백은 재도입하지 않는다(스레드 경계).

## 2. framework가 바꿔야 할 것

- DONTWAIT send가 `BACKPRESSURED`이면 반환된 completion ID(토큰)와 함께 그 코루틴을 **토큰/context 키로** 대기 목록에 넣고 suspend(패킷은 코루틴이 보유).
- framework의 poller 루프(POLLIN·POLLCOMPLETION 처리하는 그 루프)가 `POLLOUT`을 받으면 completion queue를 NO_DATA까지 pull. `WRITABLE` completion의 context로 해당 코루틴만 재개 → 같은 패킷 재전송(다시 BACKPRESSURED면 새 토큰으로 다시 대기). REQUEST completion 처리는 그대로.
- 0.16.0의 "nonzero ID = pending 수락, 나중에 SEND completion = 전송 완료" 가정 제거(이제 nonzero ID = 대기 토큰, WRITABLE = 재시도 신호).
- 같은 대상의 전송 순서가 필요하면 framework 논리 큐로 보존(Core는 순서 큐를 갖지 않음).
- `ZLINK_OPT_PENDING_MAX_*`를 SEND 목적으로 쓰는 코드 제거(REQUEST용 유지).
- 현재 EAGAIN/POLLOUT/BACKPRESSURED 처리 코드: cpp 6파일, dotnet 32, java 15, **node 0**. 정확한 영향 파일:행은 Core job 요약 `doc/plan/c016-worklog/dontwait-writable-core-summary.md`(완료 시).

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

- [x] A안(POLLOUT만) 구현 중 계약 B로 확정 → A안 job 중단, 공통 부분(payload 보관 제거·REQUEST 분리) 유지
- [ ] Core 수정 B (job c016-dontwait-writable, sol ultra+fast) — 진행 중: 대기 토큰 + WRITABLE completion + POLLOUT
- [ ] B gate (dev ctest, release-gate, wake-invariant, hotpath_gate, 바인딩 테스트) + 벤치 before/after 표
- [ ] 0.17.0 범프 커밋
- [ ] 스펙 정정 커밋(diff 사용자 확인 후)
- [ ] bindings 언어별 job
