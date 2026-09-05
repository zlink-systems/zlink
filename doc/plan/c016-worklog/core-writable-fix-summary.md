# Core REQUEST WRITABLE 수정 결과

상태: **완료 — 최종 회귀 5/5, integration 126/126, 전체 CTest 176/176 green. 남은 실패 없음. EXIT:0.**

소유 계층: Core socket REQUEST admission / pipe correlation reservation / socket-local completion queue.
Spec 조항: D-B119, `2b359d6606`의 `core/doc/spec/core/socket/README.ko.md` REQUEST DONTWAIT 절과 `27b736c96f`의 `core/doc/spec/core/systems/06-auto-hwm.ko.md` Pending request 수용 끝 문장. 두 커밋의 내용을 `git show`로 확인했으며 detached tree의 spec은 수정하지 않았다.
교차언어 대조: Framework runtime 변경 없음. 앞 job의 .NET 측 native 재현과 같은 조건을 공개 C API에서 검증했다. 공통 native Core 수정이며 bindings별 성능 재측정은 수행하지 않았다.
변경 분류: **B — 기존 Core 결함 수정**, D-B119로 확정된 자원별 wake 계약 적용. Spec gap 없음.
수정 전/후 wake 판정 규칙 수: **2 → 1**. Correlation admission 거절과 무관한 physical readiness를 따로 적용하던 판정을, **거절 원인이 된 자원의 회복만 wake 조건**으로 통일했다. 반환 순번은 이 규칙의 등록 경합을 판별하는 pipe-owned edge이며 별도 capacity·retry 정책이 아니다.

## 원인과 변경

- 원인(수정 전): `core/src/runtime/core/pipe.cpp:1699–1707`에서 request-full을 public `EAGAIN`으로 정규화한 뒤, `core/src/api/socket/socket_request_reply_submit_api.cpp:240–247`이 원인 구분 없이 공용 wait를 등록했다. `core/src/runtime/sockets/common/socket_send_complete.cpp:89–94,210–216`의 `xhas_out()` recheck가 correlation-full/physical-writable 상태에서도 WRITABLE을 즉시 발행했다. 현재 `pipe.cpp:1709–1719`의 errno 정규화는 유지한다.
- `core/src/api/socket/socket_request_reply_submit_api.cpp:113–134,191–197,265–269`: admission observer가 거절한 pipe의 lifetime reference와 반환 epoch를 보존해 submit 실패 처리까지 전달한다. 메타데이터 할당 실패도 ENOMEM으로 전달한다. Payload는 보존하지 않는다.
- `core/src/runtime/core/pipe.cpp:1565–1610,1620–1661,2786–2830` 및 `pipe.hpp`: 거절과 같은 `_out_sync` 구간에서 반환 epoch를 snapshot하고, 대기 이후 실제 reservation 반환 시에만 epoch를 전진시킨다. 기존 `release_request_correlation → send_activate_write_deferred → process_activate_write` owner mailbox 경로에서 correlation 반환 callback을 전달한다. Physical readiness와 독립된 callback이므로 timeout·disconnect의 반환도 전달된다.
- `core/src/api/socket/socket_completion_queue_internal.{hpp,cpp}`: 기존 queue-owned WRITABLE 토큰에 거절 pair/epoch를 저장한다. Physical 알림은 correlation 토큰을 건너뛰며, correlation 알림은 저장한 pair 중 반환 epoch가 바뀐 경우에만 발행한다. 한 번 발행한 토큰은 기존 FIFO에서 제거된다. WRITABLE·명시 제거 TERMINAL+ENOENT·close의 모든 종료 경로에서 pipe reference를 해제한다.
- `core/src/runtime/sockets/common/socket_send_complete.cpp:129–134,168–233` 및 `socket_base.hpp`: 토큰 등록 직후 같은 epoch predicate로 재검사한다. 반환이 등록보다 먼저 처리됐어도 놓치지 않으며, 이전 activation이 지연 도착해도 새 거절 토큰의 epoch가 같으면 발행하지 않는다. SEND·physical-credit 토큰의 기존 readiness와 종료 처리는 유지한다.
- `core/tests/integration/test_request_writable_credit.cpp`: 기존 테스트 위에서 64 KiB의 reply 전 WRITABLE 상한을 **0회**로 확정했다. Reply 후 같은 ID/context의 WRITABLE **1회**, queue NO_DATA, 전체 request 재제출 성공을 검증한다. Timeout, transient disconnect, 명시 제거, close와 ROUTER 두 pair 격리를 추가했다. 기존 `core/tests/CMakeLists.txt` 등록은 그대로 보존했다.

DEALER 대조: `core/src/runtime/sockets/internal/lb.cpp:611–639`의 general candidate 경로는 request-full pair를 이번 선택에서 제외하고 다음 후보를 시도한 뒤 복원한다. `:325–333`의 selected-pipe 경로도 ordinary scheduling에서 request-full pipe를 비활성화하지 않는다. 이 동작과 가중치는 변경하지 않았다. 현재 public REQUEST DONTWAIT 경로는 `socket_send_submit.cpp:568–593`에서 고른 candidate에 한 번 admission하며, `socket/06-dealer.ko.md:342–348`의 선택한 ROUTER 거절 계약을 유지했다.

## 최종 바이너리 회귀 분포

`test_request_writable_credit` **5/5 green**, 매 실행 11 Unity case 전부 통과. 크기·transport별 5개 표본 × 5회 = 아래 각 행 25개, 총 150개 표본. 별도 lifecycle/pair-isolation case도 각각 5/5 통과했다.

| Transport | Body | 표본 | Reply 전 BP / WRITABLE | Reply 전 성공 | Reply 후 최종 성공 | 관측 시간 min / median / max (ms) |
|---|---:|---:|---:|---:|---:|---:|
| inproc | 64 B | 25 | 0 / 0 | 25/25 | 25/25 | 0.003 / 0.004 / 0.023 |
| inproc | 4,096 B | 25 | 0 / 0 | 25/25 | 25/25 | 0.003 / 0.004 / 0.025 |
| inproc | 65,536 B | 25 | 1 / 0 | 0/25 | 25/25 | 0.007 / 0.010 / 0.023 |
| tcp | 64 B | 25 | 0 / 0 | 25/25 | 25/25 | 0.019 / 0.024 / 0.071 |
| tcp | 4,096 B | 25 | 0 / 0 | 25/25 | 25/25 | 0.018 / 0.022 / 0.073 |
| tcp | 65,536 B | 25 | 1 / 0 | 0/25 | 25/25 | 0.010 / 0.014 / 0.035 |

64 KiB의 각 표본은 reply 이후 reservation 반환에 대한 WRITABLE을 정확히 1회 받고 재제출에 성공했다. 위 시간은 submit·즉시 queue drain 관측 구간이며 50 ms의 추가 무신호 확인과 reply 대기는 제외한다. 성능 gate 측정값이 아니다. Runtime timeout·budget·retry 횟수는 변경하지 않았다.

## 게이트

- 빌드: 기존 `core/build-writable`, RelWithDebInfo, `cmake --build core/build-writable -j3` 성공.
- 관련 검증: `test_request_writable_contract`, `unittest_socket_completion_queue` 2/2 green.
- `ctest --test-dir core/build-writable -L integration --output-on-failure -j2`: **126/126 green**, 217.55 s. `test_backpressure_*`, `test_router_multiple_dealers`, single-lane suite 포함. 이후 OOM errno 보완을 최종 rebuild했고 아래 전체 gate가 최종 바이너리의 integration도 포함한다.
- 최종 `ctest --test-dir core/build-writable --output-on-failure -j2 -E hotpath_gate`: **176/176 green**, 249.08 s. 최종 바이너리의 integration 126개 포함(217.05 s·proc).
- `git diff --check`: 종료 전 최종 통과. 남은 실패 없음.
- Release+LTO `hotpath_gate`: 사용자 지시에 따라 감독자가 실행할 항목이며 이 작업에서는 실행하지 않았다.

## Hot path 영향

- `socket_request_reply_submit_api.cpp:113–134,191–197,813,924`: 성공 경로에 빈 descriptor 초기화·포인터 전달이 추가된다. Pipe reference를 담는 shared_ptr/vector 할당은 correlation 거절 경로에만 있다.
- `pipe.cpp:1565–1610,1642–1653`: 성공한 reservation의 work/count 계산·lock은 기존 그대로다. 반환 epoch의 atomic read는 거절 시, increment는 대기 이후 reservation 반환 시에만 실행한다. Pipe당 epoch 저장 공간이 추가된다.
- `socket_send_complete.cpp:168–233`: correlation 거절 토큰의 등록/recheck만 변경한다. SEND physical readiness는 동일하다.
- `socket_completion_queue_internal.cpp:80,381–416,543`: reservation에 빈 vector 저장 공간과 recycle의 clear가 추가된다. 반환/physical 알림의 대기 토큰 scan에 원인별 predicate가 추가되며, 거절 경로 밖에 새 timer·spin·poll·lock·payload copy는 없다. 성공 REQUEST의 allocation을 추가하지 않았다. 실제 처리량·latency 판정은 감독자의 Release+LTO gate가 필요하다.

작업 상태: detached HEAD `769f44fa37` 유지. 기존 테스트·CMake 변경과 `core/build`, `core/build-dev` symlink 보존, 두 symlink 미사용. Spec·Framework·bindings·doc/plan 변경 없음. Stash/checkout/reset/branch 변경/commit/push 없음.

로그: 같은 디렉터리의 `core-writable-fix-progress.md`, `core-writable-fix-final-build.log`, `core-writable-fix-repro-{1,2,3,4,5}.log`, `core-writable-fix-focused.log`, `core-writable-fix-integration.log`, `core-writable-fix-full-ctest.log`.

A용 한 줄: B/Core D-B119 correlation 반환 전 spurious WRITABLE 수정, 5회 회귀·integration 126/126 green, 전체 176/176 green, diff-check green, spec gap 없음, Release+LTO hotpath_gate 인계, EXIT:0.
