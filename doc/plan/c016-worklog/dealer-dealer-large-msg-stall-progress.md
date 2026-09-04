# DEALER-DEALER large-message stall progress

## 2026-09-04 11:12 KST — 시작/보호 확인

- 작업 트리: `main` @ `566451ca067267a3fc465f19f72ec145058d41f3`.
- 기존 사용자 변경(`AGENTS.md`, `README.ko.md`, `CONTRIBUTING*.md`) 확인; 요청 범위 밖이므로 보존.
- 금지 경로/API/ABI는 수정하지 않으며 Core source/test만 후보 범위로 한정.
- 지정 스펙의 hot-path §3·§4, polling §3, auto-HWM queue/credit 계약과 D-B64~D-B70/D-079~D-080 판정을 확인.
- 병렬 조사 시작: (1) 7개 커밋 wake 경로, (2) 결정적 회귀 테스트 위치, (3) wake invariant/기존 PUB credit 수정 비교.
- 다음 단계: 준비된 worktree의 commit/build 상태를 검증하고 4096B 우선 스크리닝 후 65536B로 원인 커밋을 이분.

## 2026-09-04 11:24 KST — 1차 이분 결과

- `1ac16a22b2`(최종 Core=`f3be895b3f`): 4096B가 30초 내 RESULT 없이 정지; client main=futex, IO=epoll. 수동 중단.
- `1344022a3e`: 4096B는 399,982 msg/s, latency mean 0.614ms, p99 2.163ms로 11초 완료. 65536B는 30초 이상 RESULT 없이 server poll 대기 상태로 정지.
- `0a002f089f`(최종 Core=`c2d5f33438`, 즉 `1344022` 직전): 65536B 동일 정지.
- `e6ae5d8fd6`(최종 Core=`04ecca54d1`, `8b40b3f` single-lane 직전이며 pull API용 perf 소스 포함): 65536B 동일 정지.
- 따라서 64KiB 최초 회귀는 `8b40b3f`/`1344022`보다 앞이며, `bb66e85376` 또는 `04ecca54d1`로 좁힘. `f3be895b3f`는 4096B까지 악화시키는 별도 증폭 단계.
- `04ecca54d1` 정확한 commit의 자체 perf는 pull API 포팅 전 소스여서 현재 헤더와 컴파일 불가; 같은 Core 상태이면서 이후 호환 perf가 들어온 `e6ae5d8fd6`로 판정.
- 다음: `bb66e85376` 대 `04ecca54d1` A/B를 호환 perf binary/runtime 조합으로 확정하고, 각 변화의 send/HWM wake diff를 좁힘.

## 2026-09-04 11:42 KST — 최초 불량 확정 및 독립 wake 회귀 재현

- `f68a74d34d`(`bb66e85376` 직전, 최종 Core=`59d6de03d3`) 자체 lib/perf: 65536B 95,336.8 msg/s, 6,247.993 MB/s, mean 122.569ms, p99 247.143ms로 약 12초 완료.
- `bb66e85376` 정확한 Core lib + pull API와 호환되는 후속 공식 perf source: 65536B가 30초 내 RESULT 없이 정지. 따라서 최초 불량 Core 커밋은 `bb66e85376`로 확정.
- `bb66e85376`은 ordinary DEALER `DONTWAIT FINAL`을 물리 HWM `EAGAIN` 경로에서 accepted pending + SEND completion 경로로 바꿨다. output/context가 NULL이어도 pending/completion을 생략하지 않으며, default pending pool은 unlimited이고 completion reservation은 socket당 65,536개다. 현재 perf는 completion을 drain하지 않고 한 socket을 EAGAIN까지 독점 submit하므로 이 계약 변화 자체가 latency/backlog 증폭의 주원인이다.
- 다만 계약 문제와 독립적으로, public `zlink_send(..., ZLINK_DONTWAIT)`만 쓰는 100-client/tcp/64KiB 회귀 테스트가 현재 Core에서 red다. 각 client는 실제 1MiB byte-HWM EAGAIN까지 15~16개를 적재했고 server는 1,597/1,597 및 1,598/1,598 전부 drain했지만, POLLOUT은 각각 80/100, 81/100만 10초 내 복구했다.
- 이 테스트는 `zlink_send_part` pending/completion 경로를 쓰지 않으므로 실제 multi-pipe byte-credit wake 누락을 분리해 실증한다. 다음: 누락 transition을 reader credit 발행 / activate_write 처리 / DEALER LB 재활성화 / poll readiness 단계로 계측해 최소 Core 수정.
- 작업 중 감독관 커밋으로 HEAD가 `566451ca06`에서 `1b83a5f314`로 전진했음을 확인했다. `566451ca06..HEAD -- core/src` diff는 없으며 감독관 변경은 보존한다.

## 2026-09-04 12:20 KST — activate_write mailbox edge 유실로 단계 축소

- 이어받은 `get_events_for_poller()`의 `schedule_if_needed()` 후보를 포함해 dev 빌드 후 재현했으나 1,597/1,597 drain 뒤 POLLOUT은 56/100만 회복했다. 후보는 근본 수정이 아니다.
- pipe 상태 계측 run: 1,595/1,595 drain, 72/100 회복. 누락 28개 전부 `out_active=0`, byte-credit waiter=1이었고 후속 DONTWAIT send는 28/28 즉시 성공했다.
- command-drain 계측 run: 1,598/1,598 drain, 67/100 회복. 누락 33개 전부 같은 상태였으며 `test_process_commands_only()` 뒤 33/33이 `out_active=1`, waiter=0으로 전환했다.
- 따라서 reader drain/credit 발행 뒤 `activate_write`는 client mailbox에 도착했지만 public poller가 그 command의 wake edge를 잃는다. activate_write 처리, DEALER LB 재활성화, pipe 자체 credit 계산은 후속 수동 drain에서 모두 정상임이 실증됐다.
- 종료 시 100개 client의 async owner/context/scheduled는 모두 0이고 async drain은 정확히 200회(연결 setup), public drain은 3,166회였다. 다음은 primary signal을 누가 소비했는지와 async-owner idle handoff 순서를 mailbox/poller 카운터로 분리한다.

## 2026-09-04 12:48 KST — 유실 가설 반증 및 회귀 테스트 판정 경계 수정

- mailbox signal까지 계측한 10초 run은 60/100 회복 후 끝났지만, 미회복 40/40 모두 client primary mailbox가 readable이고 pending hint가 켜져 있었다. 미회복군에도 `activate_write` wake가 40회 있었고, 이미 합쳐진 추가 activation은 79회였다. 즉 credit 발행과 command enqueue/signal은 성공했으며 edge가 사라진 상태가 아니었다.
- 같은 계측에서 timeout만 30초로 늘리면 100/100이 회복됐다. 마지막 POLLOUT 회복은 10,795ms였고 server가 1,597개 전체를 읽는 데 27,891ms가 걸렸다. 기존 10초 창은 server drain 전에 시작했으므로, 늦게 LWM에 도달한 client를 wake 유실로 잘못 분류했다.
- `get_events_for_poller()`의 `schedule_if_needed()` 후보는 제거했다. 정상 mailbox send/reschedule protocol을 중복하고 busy-loop 가능성만 더하므로 Core 수정 근거가 없다. mailbox/socket/pipe에 넣었던 임시 snapshot·counter·강제 command drain도 전부 제거했다.
- 회귀 테스트는 waiter를 drain 전에 등록한 채 유지하고, server가 모든 client에서 byte-LWM에 해당하는 8개 record를 읽은 시점부터 2초 POLLOUT 회복 창을 적용하도록 고쳤다. 전체 backlog를 끝까지 읽는 시간은 판정에서 제외한다.
- dev 빌드와 `test_wake_invariants` 1회가 통과했다(16.21초). 현재 repository 변경은 `core/tests/integration/test_wake_invariants.cpp` 한 파일뿐이다.

## 2026-09-04 13:14 KST — 결정적 등록 barrier와 최종 gate 완료

- 짧은 timeout으로 `zlink_poll`을 반복하면 매번 새 poller를 등록하므로 이전 등록의 wake 문제를 가릴 수 있음을 재검토에서 확인했다. 최종 테스트는 client마다 persistent public poller와 waiter를 하나씩 두고, 100개 poller 모두 `zlink_poller_size()==ZLINK_CONFIG_BUSY`로 실제 blocking wait 진입을 확인한 뒤에만 server drain을 시작한다. 각 waiter는 `zlink_poller_wait`를 정확히 한 번만 호출한다.
- server가 client별 64KiB record 8개를 읽으면 1MiB HWM의 512KiB LWM을 넘는다. 마지막 client가 이 경계를 넘은 시점과 마지막 POLLOUT 회복 시점의 차이가 2초 미만인지 검사한다. 전체 backlog drain 시간은 더 이상 wake deadline에 포함하지 않는다.
- 최종 repository diff는 `core/tests/integration/test_wake_invariants.cpp` +366줄뿐이다. Core source/API/ABI, framework, bindings, doc, hotpath reference는 바뀌지 않았고 임시 계측은 없다.
- dev: `build-core.sh dev` 통과, 최종 `test_wake_invariants` 15.37초 통과. release-gate LTO build 통과.
- release 전체: 140/140 통과(184.30초). `wake-invariant` 4-test label을 추가 5회 실행해 20/20 통과(각 17.52, 16.87, 16.91, 16.99, 16.68초).
- standalone hotpath: 4/4 통과. 기준 대비 ratio는 DEALER/DEALER inproc 1.0022, DEALER/ROUTER req/rep 1.0068, PAIR inproc 1.0032, ROUTER/ROUTER tcp 1.0001이다.
- raw header mirror는 12/12 일치, `git diff --check` 통과.
- 참고 multi runner는 결합 실행이 첫 cell에서 멈춰 뒤 size를 볼 수 있어 size별로 26초 제한을 걸었다. 4096B와 65536B 모두 DEALER_DEALER tcp 본 실행에서 RESULT 없이 timeout(124)했으며 종료 뒤 잔류 프로세스는 없다. 이는 `bb66e85376` 이후 completion을 drain하지 않는 benchmark 계약 문제와 일치한다.
- 주의: 테스트 파일의 byte-buffer `zlink_send`는 exported symbol이 아니라 `testutil_unity.hpp`의 compatibility shim이며 valid socket에서는 `send_msg_internal`을 호출한다. 따라서 이 테스트가 고정하는 범위는 pull-completion 경로와 독립인 synchronous DONTWAIT physical-HWM 경로와 public poller의 POLLOUT 회복이다.
