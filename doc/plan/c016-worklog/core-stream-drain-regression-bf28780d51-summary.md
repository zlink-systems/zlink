# Core STREAM drain 회귀 진단 및 수정

- 기준: `bf28780d5147456c9a7871fb89acd51fb3c40d17`; 부모 `1a08cb7c769505bdc0aba499b054604f37fe6b08`.
- 작업공간: `/home/hep7/project/zlink-core-a`, detached HEAD. Commit 없음.
- 산출물: `/home/hep7/project/zlink-core-a/core-stream-drain.patch`.
- 실행 증거: `/home/hep7/project/zlink-core-a/stream-drain-logs/`. 제보 로그의 관련 구간은 source 경로와 line을 붙여 `symptom-evidence.txt`에 보존했다.

## 결과

`bf28780d51`은 partial packet을 처리하는 한 번의 receive/poll 진입에서 raw input 전체를 소비할 수 있게 바꿨다. 공개 C API 재현에서 context shutdown이 drain 도중 요청돼도 HEAD는 남은 fragment를 모두 소비한 뒤 종료를 반환했다. 같은 재현에서 부모와 수정안은 buffered input을 남긴 채 종료를 반환했다. 같은 poller의 이미 준비된 timer에 대한 진행 경계도 HEAD에서 사라졌다.

Core packet pump에 64-chunk 처리 경계와 기존 receive-progress/mailbox wake를 함께 적용했다. Queue가 비었을 때만 조립하는 정책과 post-pop refill 제거는 유지한다. 부모로 단순히 되돌리면 이미 도착한 입력의 readiness가 유실되므로 revert하지 않는다.

다만 제보된 Framework gate 실패가 이 drain 회귀 때문이라는 인과관계는 확인되지 않았다. 실제 로그의 node 종료 원인은 stale RID disconnect 예외이고, dotnet A1은 target route의 `NotConnected`를 담은 오류 응답이다. node deadline assertion은 mock transport 경로다. 이 패치를 세 gate의 해결로 판정해서는 안 된다.

## 증상별 sequence와 근거

### node ZoneWorld

`/dev/shm/zlink-tmp-node/zlink-zoneworld-I8r3Nz/logs/gateway.log`:

1. `topology=ready` 뒤 player-a1, player-a3, player-b1-west의 session bind가 기록된다.
2. `RawStreamSessionService.deliver`의 STREAM submit 실패 처리에서 `disconnectRid`를 호출한다.
3. `disconnect_rid failed: No such file or directory`, `ConfigError`, `nativeErrno: 2`, `result: 706`이 처리되지 않아 process가 종료된다.

호출 근거는 `framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-mesh-backend.ts:1997`과 `:2018`이다. `NotConnected`/`NotFound`/`Terminated` submit 뒤 같은 RID를 disconnect하는 경로에서 추가 오류가 발생한다. Core의 missing-route `ENOENT` 경로는 `core/src/runtime/sockets/stream/stream.cpp:368` → `core/src/runtime/sockets/common/socket_base_routing.cpp:216`이며 `bf28780d51`이 이 경로를 변경하지 않았다. 로그는 receive pump hang이나 packet 역전을 증명하지 않는다. 정확한 send 실패 시점과 route retirement의 인과 추적은 별도 owner 조사 항목이다.

### dotnet ZoneWorld run 2, A1와 G3

보존된 run: `/dev/shm/zlink-tmp-dotnet/tmp.9F42jocQ1u/logs/`.

1. `client.log:3`에서 최초 A1은 통과한다.
2. `run_sample.sh:978` 이후 G3는 zone-node-2를 교체하고 새 RID를 확인한 뒤 `run_client ZW-A1`을 실행한다. `:990`의 조건은 RID 변경과 A1 성공의 AND이다. 따라서 G3 실패 문구만으로 RID 미게시를 결론낼 수 없다.
3. `ops.log:2656`에서 교체 RID `zn-db52a991-c5f8-499d-ba4a-fc506f721174`가 게시되고 `:2835`, `:3014`, `:3196`에서 다시 관측된다. 앞선 RID는 `:2209`의 `zn-7cab6850-4555-4699-99bd-4dbf9c497eaa`다.
4. `zone-node-replacement.log:1625`에서 actor `a1-81354a`의 `JoinSpot zone-nw`가 `Unavailable`, “target route is not connected”, inner `ZlinkRequestException` code 109로 실패한다. 호출은 `ZLinkActorRemoteJoiner.TryRequestCanonicalAdmissionAsync:1064`다.
5. 같은 로그 `:1636`에서 `flow=01a073de-cac0-744a-a7df-52aa3891993e`의 JoinSpot failure가 기록되고, `:1639`에서 같은 flow의 `JoinWorldRes`가 08:19:50.968에 전송된다. 실패 기록은 08:19:50.957이다.
6. `client.log:45`의 A1 assertion은 `Scenarios.cs:96`의 `join.Error is null` 검사다. 따라서 admission notification과 STREAM reply의 순서 역전을 검사한 실패가 아니다. 오류 응답을 받았다는 증거다. 이 A1 실패가 G3의 합성 실패 조건을 충족한다.

STREAM packet 순서 위반과 새 RID 게시 실패는 이 로그로 확인되지 않는다. target route가 연결되지 않은 이유는 별도 ROUTER/Framework join 경로 진단이 필요하다. 해당 파일들은 수정하지 않았다.

### node bind deadline contract

`framework/languages/node/test/contract/stream-actor-bind-replay.test.js:27`의 fixture는 `requestService`를 mock으로 제공하고 `:64`의 actor lookup도 미리 성공시킨다. 실제 native STREAM packet receive/poller를 실행하지 않는다. `:35`에서 `Date.now()`를 기록하고 `:87`에서 별도로 계산한 deadline과 `:108`에서 ±1 ms로 비교한다.

따라서 해당 assertion은 Core packet drain의 직접 회귀 재현이 아니다. rebuild 중 부하와 JS clock sampling 사이의 간격이 실패에 기여할 가능성은 있지만, 실패 당시 clock sample/스케줄링 기록이 없어 원인으로 확정하지 않는다. assertion이나 Framework runtime을 변경하지 않았다.

## Core 원인과 계약

- 원인: `bf28780d51:core/src/runtime/sockets/stream/stream.cpp:714`의 `pump_packet_receive_queue()`가 raw chunk budget을 없애고 queue가 비어 있는 동안 계속 처리한다. 부분 packet만 있으면 처리량이 전체 backlog 또는 계속 유입되는 입력에 종속된다.
- 실행 thread: `recv_packet → socket_base_t::recv → xrecv → pump` 또는 `poller → get_events_internal → has_in → xhas_in → pump`를 실행하는 public caller다. pump가 I/O thread에서 transport read를 수행하거나 네트워크 입력을 blocking wait하는 것은 아니다.
- 동기화: poll의 `socket_base_api.cpp:1003`과 async owner가 있는 receive의 `socket_base_msg.cpp:100`은 receive sync를 pump 전체에 유지한다. async command dispatch도 `socket_base_lifecycle.cpp:534`에서 같은 sync를 획득한다. async owner가 없는 receive는 public receive lease를 유지하고 pump에서 돌아온 뒤 command 처리를 한다. Pipe 하나의 lock을 전체 loop에 유지하는 구조는 아니다.
- 다른 peer: `fq.cpp:363`의 raw-message round-robin은 그대로다. 이미 활성인 peer의 작은 complete packet은 긴 partial packet 사이에서 처리된다. 아직 적용되지 않은 activation/termination command와 다른 poller source가 진행할 경계가 사라진 것이 문제다.
- packet 순서: per-pipe decoder와 FIFO packet queue는 변경되지 않는다. 다른 RID의 enqueue interleaving은 달라질 수 있지만 §6.3은 Core queue에 들어간 순서를 보장하며, wire의 전역 도착 순서를 보장하지 않는다. readiness는 level이며 packet마다 정확히 한 번 발생하는 event 계약이 아니다. record는 한 번 dequeue되고 queue가 남으면 POLLIN도 유지된다.

**소유 계층:** Core STREAM packet assembly/progress. Binding과 Framework에 상태나 보정 로직을 추가하지 않는다.

**Spec 조항:** `core/doc/spec/core/socket/08-stream.ko.md` §6.2의 blocking receive 종료, §6.3의 complete-packet readiness/ordering/bounded queue; `core/doc/spec/core/05-polling.ko.md` §3의 level wake/no-lost-wake. §3에는 숫자로 된 command-progress 시간/청크 상한이 없다. 64는 구현의 처리 경계이며 공개 SLA로 주장하지 않는다. 테스트는 backlog 전체 소비를 다른 source/종료 처리의 전제조건으로 삼지 않는지를 관측한다.

**교차언어 대조:** node `bindings/node/native/src/addon_core.cc:2986`, dotnet `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketKernel.Stream.cs:32`가 같은 `zlink_stream_recv_packet`을 호출한다. 공통 Core 수정이며 언어별 runtime 변경은 없다. 제보된 node mock contract는 이 경로 밖이다.

**변경 분류:** B — Core packet pump의 진행 경계 회귀. 제보된 Framework 실패 원인의 분류/수정 승인을 대신하지 않는다.

## 수정과 대안

| 대안 | 판정 |
|---|---|
| 부모의 64-chunk 반환과 post-pop refill로 단순 revert | 거부. 공개 API에서 미소비 buffered fragments가 남은 채 poll/recv가 timeout된다. |
| 무제한 drain 유지 | 거부. shutdown과 다른 poller source의 진행 전에 전체 partial backlog를 처리한다. |
| empty-queue pump 한 곳에서 bounded step + continuation | 적용. command/다른 source에 진행 경계를 주고 이미 받은 입력도 계속 처리한다. |

`stream.cpp:723`에서 budget에 도달하면 기존 `notify_receive_progress_locked()`로 blocking receiver의 epoch를 진행시키고 기존 `mailbox_t::signal()`로 command wait/public poller를 깨운다. 둘은 이미 있는 서로 다른 대기 경로다. 미완성 packet에 `POLLIN`을 게시하거나 새 queue/state/command/timer를 만들지 않는다. 기존 `notify_receive_progress_locked` 선언은 `socket_base.hpp:1023`의 protected 영역으로 옮겨 재사용한다.

수정 전/후 규칙 수: refill 결정 owner는 **1 → 1**이며, 부모의 두 결정 경로와 비교하면 **2 → 1**이다. 동일 owner의 진행 규칙을 “packet 또는 input 끝까지 계속”에서 “packet/input 끝 또는 bounded step에서 continuation 후 반환”으로 교체했다. Persistent state 추가는 0이다. Post-pop refill은 복구하지 않았다.

변경 파일:

- `core/src/runtime/sockets/stream/stream.cpp`
- `core/src/runtime/sockets/common/socket_base.hpp` — 기존 함수의 접근 범위만 이동
- `core/tests/CMakeLists.txt`
- `core/tests/integration/test_stream_packet_progress.cpp`
- `core/tests/integration/test_stream_socket.cpp` — B가 추가한 private pipe 주입 테스트를 공개 API 테스트로 대체

기존 private `xhas_in()` 한 번에 모든 fragment를 decode해야 한다는 검사는 공개 poller의 진행 계약을 표현하지 않는다. 테스트의 성공 조건을 낮추지 않고 공개 API에서 완전한 입력의 wake와 payload를 검증하도록 옮겼다. 새 integration 파일은 `zlink.h`와 public monitor/poller/recv/ctx API만 사용한다. WebSocket message마다 1-byte fragment를 보내므로 TCP coalescing이나 internal failpoint에 기대지 않는다.

## 재현 및 검증

부모 source는 `git archive HEAD^ core VERSION LICENSE`로 worktree 안 `stream-drain-logs/parent-source/`에 추출하고 `core/build-parent`에서 별도로 빌드했다. Main의 `core/build-dev`는 사용하지 않았다. 부모/HEAD 실험은 같은 public test object를 각 버전 shared library에 연결했다.

| 공개 API case | 부모 | bf28780d51 | 수정안 |
|---|---|---|---|
| 519개 buffered fragments, poller, monitor owner 있음/없음 | timeout | PASS | PASS |
| 같은 입력의 blocking recv, monitor owner 있음/없음 | NO_DATA/timeout | PASS | PASS |
| 두 session: 긴 partial packet + 순서 있는 작은 packet 16개 | 작은 packet 이후 큰 packet readiness timeout | PASS | PASS |
| idle 뒤 마지막 fragment 도착 | PASS | PASS | PASS |
| 이미 준비된 timer + partial backlog | timer 진행 후 나머지 packet readiness timeout | timer 전에 backlog 전부 소비 | PASS |
| drain 도중 context shutdown | PASS | 종료 전 backlog 전부 소비 | PASS |

대표 관측:

- HEAD shutdown 요청 시 262,152 chunks, receive 반환 시 **0**. `shutdown-head.log`.
- 부모는 같은 요청 후 반환 시 **261,961** chunks. `shutdown-parent.log`.
- 수정안은 같은 요청 후 반환 시 **261,833** chunks. `repro-fixed.log`.
- HEAD timer 재현은 전체 partial input 소비, 수정안은 8,073 chunks를 남긴 채 timer 반환. 시간 수치는 host scheduling에 종속되며 pass 조건은 ms 상한이 아니다.



## Throughput

같은 executable을 `LD_LIBRARY_PATH`로 부모/HEAD/수정안 shared library에 연결했다. `bench.cpp`는 TCP client가 64 KiB packet을 보내고 Core의 packet recv 뒤 1-byte ACK를 받는 직렬 loop다. `bench-pipeline.cpp`는 같은 입력을 연속 전송하고 마지막 수신 뒤 ACK를 받는다. 둘 다 100회 warmup 뒤 100,000건을 측정하고, 실행 순서를 바꿔 3회씩 측정했다. 측정 중 이 job의 빌드와 테스트는 실행하지 않았다.

| 64 KiB TCP, 중앙값 ops/s | 부모 | bf28780d51 | 수정안 | 수정안 / HEAD | 수정안 / 부모 |
|---|---:|---:|---:|---:|---:|
| 직렬 packet/ACK loop | 15,419.47 | 15,431.52 | **15,148.10** | -1.84% | -1.76% |
| 연속 입력 throughput loop | 72,408.70 | 75,899.05 | **74,226.99** | -2.20% | +2.51% |

근거: `stream-drain-logs/throughput-long.log`, `throughput-pipeline.log`; 재현 소스와 executable도 같은 디렉터리에 보존했다. 초기 3,000건 측정은 run이 약 0.2초로 짧아 100,000건 측정으로 평가했다. 같은 host/no-LTO quick loop에서는 HEAD 대비 차이가 두 경우 모두 5% 안이며, 연속 입력의 부모 대비 중앙값 증가는 남아 있다. 측정 분산이 있으므로 2.51%를 통계적으로 확정된 개선이라고 주장하지 않는다.

원 커밋 메시지의 `533 → 6979 ops/s`는 이 quick loop와 다른 측정이다. 그 수치를 재현했다고 주장하지 않으며, 이번 결과를 release LTO/Framework end-to-end 성능 gate로 대체하지 않는다.

## Gate 결과

| 검증 | 결과 | 로그 |
|---|---|---|
| `nice -n 10 env JOBS=4 scripts/build-core.sh dev` | PASS | `build-final.log` |
| 신규 integration executable `--repeat until-fail:10` | **8 cases × 10 PASS**, 4.76 s | `repeat-fixed.log` |
| `ctest -R '^test_stream'` | **17/17 PASS**, 6.96 s | `stream-suite.log` |
| `ulimit -v 16777216; ctest -j2 -E '^hotpath_gate$'` | **182/182 PASS**, 240.20 s, 전체 1회 | `full-ctest.log` |
| `git diff --check` | PASS | 작업공간 실행 |

Core gate의 남은 실패는 없다. 부모/HEAD 비교에서 발생한 실패는 위 표의 회귀 재현 결과이며 수정안의 잔여 실패가 아니다.

Patch 생성 명령:

```bash
git add -N core
git diff HEAD -- core > /home/hep7/project/zlink-core-a/core-stream-drain.patch
```

Patch SHA-256: `bab5e5015682d55c464da603c9e7c00a883c42a2e8dbca4fa77c4d36071e2767`.

비교 shared library SHA-256:

- 부모: `29cefd190e4400c84bfe53bb7d9ae750b381a87bdf000f0c7559d6ad4e58b37c`
- HEAD: `bb87487426bfb42e65913c23427275a81283d8c52749864ae579e5c9df486c9e`
- 수정안: `e6568d790d4a1a4bd9ab3c1dfa112c8e1fe176b73117542bc531ebce0393b4cb`

모두 이 작업공간에서 빌드한 RelWithDebInfo/no-LTO 라이브러리다. 제보의 rebuild13 library hash와 동일한 artifact라고 주장하지 않는다.

## BLOCKERS

- 이 Core 수정으로 node/dotnet Framework gate가 해결됐다는 증거는 없다. 해당 gate를 재실행하거나 Framework/binding 파일을 변경하지 않았다. 제보된 실패는 위의 각 owner 경로에서 별도로 판정해야 한다.
- polling §3에는 숫자 command-progress bound가 없다. 이 패치의 64-chunk 경계를 public latency SLA로 간주하면 안 된다. 별도 spec 변경은 하지 않았다.
- `hotpath_gate` 및 binding/package gate는 요청한 `-E '^hotpath_gate$'`와 작업 범위에 따라 실행하지 않는다. 이번 quick loop는 release 성능 판정 전체를 대신하지 않는다.
