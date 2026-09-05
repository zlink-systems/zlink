# C++ Bingo·TicTacToe cleanup 137 진단

## 판정

**Core의 close와 completion-poller 해제 사이의 경쟁이다. `7cbf12de41`이 기존
close handoff 결함을 샘플 종료 경로에서 드러낸 회귀 경계다. 변경 분류는 B — 기존 결함이다.**
Framework host가 ClientServer DEALER를 닫는 동안 Core가 이미 close를 수락한 socket에
async executor를 다시 시작한다. Close waiter가 기다리는 `async_processing_done`이 다시
false가 되어 `zlink_close()`가 반환하지 않는다. Runner는 이후 SIGKILL을 보내고 137을 기록한다.

`zlink_ctx_term()` 대기, 열린 monitor lease, ROUTER HANDOVER standby pipe 또는 runner의
zombie 판정 오류로 재현된 실패가 아니다. 아래 공개 C API 재현은 Framework·binding·TCP·
ROUTER·HANDOVER 없이 같은 stack과 내부 상태를 만든다.

- **소유 계층:** Core socket lifecycle / completion owner handoff. C++ binding은 Core poller를
  해제하는 호출자이며 Framework는 logical host drain과 transport 정리를 소유한다.
- **Spec 조항:** `core/doc/spec/core/socket/README.ko.md:601-621` §6 `zlink_close`;
  `core/doc/spec/core/05-polling.ko.md:43-49` §3의 등록 socket close → `POLLERR`,
  `:127-136` §5의 remove 전 socket close 허용과 lifetime pin;
  `core/doc/spec/core/01-context.ko.md:47-61` §3;
  `framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md:759-783` §14.
- **교차언어 대조:** Java 1회는 네 role 모두 `STOPPED/NONE`과 정상 SIGTERM 종료를 확인했다.
  .NET·Node runner는 exit 0이지만 role SIGKILL을 숨기므로 정상 종료 parity의 근거가 아니다.
- **변경 분류:** **B**. Stage 1 진단만 수행했다. 구현·test·spec 수정과 commit은 없다.

## 현재 패키지 재현

모든 C++ 실행은 기존 `framework/languages/cpp/build/linux-ninja-c-e2e`의 바이너리를 사용했다.
Runner가 수행하는 configure/build 뒤에도 주요 sample 바이너리 SHA-256은 처음과 같았다.
각 실행의 `TMPDIR`는 `/dev/shm/zlink-tmp-cpp/diag-cleanup-137/<실행명>`으로 격리했다.
기존 sample의 `message_flow(normal)`와 file logger를 사용하고
`ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1`을 켰다. 추가 진단 실행에서는 기존
`ZLINK_CPP_HOST_STOP_TRACE=1`도 켰다. 외부 wrapper가 삭제 전 flow·role log·config와
runner xtrace를 복사했다. Runtime trace sink를 추가하지 않았다.

| 현재 패키지 실행 | Runner exit | Application | 실제 cleanup |
|---|---:|---|---|
| `current-ttt-1` | 0 | 완료 | 모든 role exit 0; Play-b drain 약 10초 |
| `current-ttt-2` | 1 | `tictactoe=completed` | Play-b 67515, Play-a 67516 SIGKILL / 137 |
| `current-ttt-3` | 1 | `tictactoe=completed` | Play-b 70673 SIGKILL / 137 |
| `current-bingo-1` | 1 | 완료 | Session-b 75561 SIGKILL / 137 |
| `current-bingo-2` | 1 | 완료 | Session-a 80439, Session-b 80440 SIGKILL / 137 |
| `current-bingo-3` | 0 | 완료 | 모든 role exit 0 |

따라서 두 sample 모두 **2/3 실패**다. `gdb`를 cleanup 시작 약 4초·16초에 붙여 같은 정지
위치를 확인했다. `current-ttt-state` 추가 실행도 137이며 lifecycle 상태를 함께 보존했다.

대표 stack은 `current-ttt-2/gdb-67515-16.txt:2102-2128`이다.

```text
main
  app_t::run                                      app.cpp:2736
  app_state_t::stop_hosted_services               app.cpp:336,367
  location_auto_connect_host_service_t::stop
  client_server_location_runtime_t::stop_clients  ...runtime.cpp:1709
  raw_client_server_client_t::close               raw_client_server_owner.cpp:951
  dealer_socket_t / socket_t destructor
  zlink_close
  socket_base_t::complete_close_handoff
  socket_base_t::finish_close_handoff             socket_base_api.cpp:163
  socket_lifecycle_coordinator_t::complete_deferred_close_handoff
                                                  socket_lifecycle_runtime.cpp:685
  socket_lifecycle_coordinator_t::wait_async_quiesced(-1)
                                                  socket_lifecycle_runtime.cpp:652
```

Bingo Session도 같은 `raw_client_server_client_t::close → zlink_close →
wait_async_quiesced(-1)`에서 멈춘다. 다른 thread의
`app_t::run_shared_shutdown`, `app.cpp:3816`은 `teardown_complete`를 기다리고 있다.
Framework가 `public_host_runtime_t`의 stop/drain에서 먼저 교착한 경우가 아니다.

`raw_client_server_owner.cpp:927-951`에서 application receive-flow 등록, port,
monitor poller, monitor를 정리한 다음 DEALER를 파괴한다. 이 시점의 Core snapshot은 다음과 같다.

| 값 | 실패 snapshot |
|---|---|
| `public_api_state` | `0x8000000000000000` — close accepted, public in-flight 0 |
| `async_mailbox_active` | true |
| `async_quiesce_pending` | false |
| `async_processing_done` | false |
| `async_quiesce_completed` | false |
| `monitor_runtime.owns_async_command_processing` | false |
| `_completion_poller_refs` | 0 |
| `_transport_pair_owner_progress_refs` | 0 |
| `_async_command_processing_retained` | true |
| mailbox `_scheduled`, `_command_pending_hint` | false, false |
| `LINGER` | 0 |
| pending WRITABLE wait | 1 |

`current-ttt-state/gdb-91715-4.txt`와 `-16.txt`에서 이 상태가 유지된다. 새 executor가
유휴 상태로 유지되므로 close waiter를 끝낼 추가 stop completion이 없다.

## Core만 교체한 비교

최초 old 기준은 `git log --oneline -1 7ffb8e55d9^`가 반환한 **`a0157dc270`**이다.
현재 Framework·C++ binding 바이너리를 그대로 두고 실행별 `ZLINK_LIBRARY_PATH`와
`LD_LIBRARY_PATH`만 바꿨다. C++ 실제 로드 경로는 각 role의 `/proc/<pid>/maps`와
GDB shared-library 목록으로 확인했다.

| Core | TicTacToe | Bingo | Framework 없는 공개 C API 재현 |
|---|---|---|---|
| 설치 패키지 `a19fc219…` | 1/3 정상 종료 | 1/3 정상 종료 | 첫 실행 iteration 0에서 close 정지; 별도 stack 실행도 동일 |
| OLD `a0157dc270`, 독립 재빌드 | 1/1 정상 종료 | 1/1 정상 종료 | 이 버전에서는 별도 실행하지 않음 |
| `7cbf12de41^` = `1c1bfdade3` | **3/3 정상 종료** | **3/3 정상 종료** | **300/300 완료**, exit 0 |
| `7cbf12de41`, 같은 격리 snapshot에 commit 적용 후 재빌드 | 2/2 정상 종료 | 2/2 정상 종료 | **iteration 0 close 정지**, exit 3 |

`1c1bfdade3`에는 `1c69086a4a`, `8b82d51b75`, `0c39ed2e52`가 모두 포함되어 있다.
따라서 이 세 commit을 각각 되돌릴 필요 없이 `7cbf12de41`의 두 runtime 파일 변경으로
공개 API 실패가 다시 나타나는 것을 확인했다. 순수 `7cbf12de41` Core로 실행한 sample
4회에서는 137을 재현하지 못했다. 따라서 sample의 commit별 실패율을 확정하지 않는다.
회귀 commit 판정은 같은 공개 API 재현의 직전/직후 결과와, 설치 package sample에서 직접
포착한 동일한 restart/close stack에 근거한다.

| Native library | SHA-256 |
|---|---|
| 설치 패키지와 main `core/build-dev` | `a19fc2194633424b117bed9e9aa8352ea6dd4310ab3bfa9554898bbfa388eda3` |
| OLD clean build | `2c65ea9ba17d3abd667b7092ef093e2ff5395608cd750f1a5d35619fdaa8bd36` |
| `7cbf12de41^` 격리 build | `92ac3c5a01a5491c24bfebc940d75f76baab2edcdcc6ebc87739cd4aebe4cfb2` |
| `7cbf12de41` 격리 build | `6bfdc634d8e27a265a8612589f5539bdb8abf3ce0d7ea6c2a4f722a49a4b96c7` |

원래 요청한 `nice -n 10 env JOBS=4 scripts/build-core.sh dev`를 `zlink-core-b`에서
실행했다. 최초 build에는 static archive/일부 test 링크 오류와 clock-skew 경고가 있었고,
그 산출물의 첫 sample은 cleanup SIGSEGV 139였다. **이 실행은 유효한 old 비교에서 제외했다.**
이후 `--target libzlink --clean-first -j4`로 재빌드한 old library는 11:22:32에 완성되었고
위 두 sample이 통과했다. Main Core와 package는 재빌드하지 않았다.

11:23:24에 다른 job이 `zlink-core-b` HEAD를 `7ffb8e55d9`로 변경한 것이 reflog에 남았다.
이후 공유 build의 산출물은 비교에 사용하지 않았다. Commit snapshot을
`/home/hep7/project/zlink-core-b/core/build-diag-cleanup-137/pre-monitor`에 추출하고
별도 `core/build-dev`에서 동일한 RelWithDebInfo, LTO OFF, tests ON, `JOBS=4`로 library만
빌드했다. 직전 library를 보존한 뒤 commit의 변경 파일을 snapshot에 적용하고 다시 빌드했다.
두 runtime 파일의 Git blob hash는 `7cbf12de41`과 같다. `zlink-core-a`와 병행 .NET 작업은
수정하지 않았고, `zlink-core-b`에 원래 있던 test 파일 삭제도 복원하거나 정리하지 않았다.

## 정확한 재활성화 경로

`current-ttt-restart-2/restart-16520.txt`는 GDB breakpoint를 통해 **close accepted 뒤의
executor 시작**을 직접 포착했다. Breakpoint는 값을 관찰하고 계속 실행했으며 바꾸지 않았다.

```text
C++ binding completion_owner_t::runtime_loop
  completion_owner_t::shutdown
  completion_owner_t::stop_runtime_owner_locked   completion_owner.cpp:721-737
  zlink_poller_destroy                            poller_api.cpp:133
  release_poller_registration                      poller_registration.cpp:283
  socket_base_t::release_completion_poller         socket_base_dispatch.cpp:289
  socket_base_t::resume_completion_processing_if_needed
                                                   socket_base_api.cpp:950
  socket_base_t::ensure_completion_processing       socket_base_dispatch.cpp:177
  socket_base_t::start_async_mailbox_processing     socket_base_lifecycle.cpp:890
  socket_lifecycle_coordinator_t::start_async_mailbox_processing
                                                   socket_lifecycle_runtime.cpp:580-586
```

인과 관계는 다음과 같다.

1. `7cbf12de41`의 `socket_base_dispatch.cpp:221-245`, `socket_base_lifecycle.cpp:918-927`은
   monitor가 command lease를 소유하면 completion poller를 추가해도 async executor를 유지한다.
   이 변경의 의도인 monitor progress 자체는 유지해야 한다.
2. 종료 시 monitor를 닫아 lease는 해제된다. 남아 있는 completion poller 때문에 executor가
   유지된다. `zlink_close`는 close admission을 확정하고 async owner의 quiescence를 기다린다.
3. C++ binding completion thread는 닫히는 socket의 poller event/error를 받고 자신의 poller를
   파괴한다. `release_completion_poller`는 참조를 1→0으로 바꾼 다음 무조건 resume 경로를 호출한다.
4. `socket_base_api.cpp:926-950`은 close 상태를 확인하지 않는다. Pending WRITABLE wait가
   있으면 `ensure_completion_processing`을 호출한다. 이 함수도 close admission을 확인하지
   않고 이전 quiescence가 끝난 뒤 새 executor를 시작할 수 있다.
5. `socket_lifecycle_runtime.cpp:580-586`은 새 executor를 설정하면서
   `async_processing_done=false`, `async_quiesce_completed=false`로 되돌린다.
   `:644-655`의 close waiter는 이전 handoff 완료가 아닌 이 재사용 가능한 boolean을 기다린다.
   늦게 깨어나면 새 executor 뒤에서 무기한 대기한다.

GDB에서 restart 직전 `public_api_state`는 closing bit만 설정되어 있고,
`async_quiesce_pending=false`, `async_processing_done=true`였다. 즉 이전 handoff는 끝났다.
한 관찰에서는 `reaper_poller_value`도 이미 설정되어 있었다. 따라서 단순히 wait predicate를
바꿔 반환시키는 조치는 executor와 reaper의 동시 소유 가능성을 남긴다.

Sample의 `state=stopped` flow record도 process 종료 증거가 아니다. `app.cpp:3784` 부근에서
해당 state를 게시한 뒤 `:3816`에서 실제 teardown을 기다린다. Runner의 30초 관찰 시간을
늘리거나 binding destructor의 순서를 바꾸어 경쟁을 피하는 것은 이 Core 결함의 수정이 아니다.

## 공개 API 재현과 회귀 제안

기존 source/test 파일은 만들거나 바꾸지 않고 C++ compiler의 stdin으로 아래 API 순서를
실행하는 임시 진단 binary `diag-cleanup-137/public-close-race`를 만들었다. 이 binary는
공개 `zlink.h`와 `libzlink.so`만 사용한다. Framework·C++ binding library는 링크하지 않는다.

```cpp
// 각 반복마다 새 context와 연결하지 않은 DEALER를 사용한다.
auto ctx = zlink_ctx_new();
auto s = zlink_socket(ctx, ZLINK_SOCKET_DEALER);
int linger = 0;
zlink_set_option(s, ZLINK_OPT_LINGER, &linger, sizeof linger);
zlink_socket_monitor_open_options_t options{};
options.events = ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED;
auto monitor = zlink_socket_monitor_open(s, &options);
auto poller = zlink_poller_new();
zlink_poller_add(poller, s, nullptr, ZLINK_POLLCOMPLETION);

zlink_msg_t msg;
zlink_msg_init_size(&msg, 1);
zlink_completion_id_t token = 0;
// BACKPRESSURED + nonzero WRITABLE wait token. 성공한 SEND가 아니다.
auto submitted = zlink_send_part(s, &msg, ZLINK_SEND_FLAGS_DONTWAIT,
                                ZLINK_PART_FINAL, nullptr, &token);
assert(submitted == ZLINK_SUBMIT_BACKPRESSURED && token != 0);
zlink_msg_close(&msg);
zlink_monitor_close(&monitor);

std::thread closer([&] { zlink_close(s); });
zlink_poller_event_t event{};
zlink_config_result_t error = ZLINK_CONFIG_OK;
zlink_poller_wait(poller, &event, 1, 1000, &error); // POLLERR=4
zlink_poller_destroy(&poller);
closer.join(); // 현재 package / 7cbf12de41에서 무기한 대기 가능
zlink_ctx_term(ctx);
```

실행 binary는 모든 생성·설정·submit·close 반환값을 확인한다. Close가 200ms 동안 반환하지
않으면 `CLOSE_STUCK`을 기록하고 10초간 stack 수집 시간을 준 뒤 진단 exit 3으로 끝난다.
이 값은 runner의 SIGKILL/137과 구분한다. 직전 Core의 최초 30초 관찰은 244번째 반복 전에
외부 timeout으로 끝났으므로 pass로 세지 않았다. 별도 300회 전체 관찰은 exit 0으로 완료했다.
현재 package와 순수 `7cbf12de41`은 iteration 0에서 정지했다.
`public-close-race-state.txt`에는 sample과 같은 close stack, closing bit,
`active=true/pending=false/done=false`, monitor/poller 참조 0, WRITABLE wait 1이 남아 있다.

최소 수정 위치는 **Core `socket_base_dispatch.cpp:256-289` / `socket_base_api.cpp:926-950`의
completion owner 반환 경계와 `ensure_completion_processing`의 lifecycle 직렬화**다.
Close가 accepted된 socket에는 기존 pending completion을 정리하면서 새로운 executor를
설치하지 않아야 한다. Close admission과 owner start의 경쟁까지 기존 gate로 닫아야 하므로
락 밖의 boolean 검사 한 번만 추가하는 것으로 충분하다고 단정하지 않는다.

두 대안을 비교하면, close 상태를 owner handoff에서 보장하는 방법이 책임 경계에 맞는다.
`wait_async_quiesced`의 predicate만 바꾸는 방법은 위 reaper/executor 중복 가능성을 남긴다.
Monitor lease 변경을 되돌리거나 Framework에서 poller/close 순서를 재조립하는 것도 채택하지 않는다.

Stage 2 회귀는 위 공개 API 재현을 Core integration test로 옮기고 monitor 유무,
WRITABLE wait 유무, `poller_remove`/`poller_destroy`, close-before-remove와 경쟁을 검사해야 한다.
Close 완료, poller의 단일 `POLLERR`, completion 정리, context term 완료와 close 뒤 executor
재시작 부재를 검증한다. Monitor progress 회귀도 유지해야 한다. 수정 뒤 두 C++ sample의
application 완료와 모든 role 종료를 함께 검증한다. **이번 Stage 1에서는 구현하지 않았다.**

## 교차언어 종료 결과

모두 현재 설치 Core `a19fc219…`를 선택하도록 `ZLINK_LIBRARY_PATH`와 `LD_LIBRARY_PATH`를
설정했다. .NET·Java는 해당 환경 변수를 사용하는 loader 계약을 따른다. Node는 Linux dynamic
linker의 `LD_LIBRARY_PATH`를 사용하며 같은 환경의 `ldd zlink.node`도 현재 설치 Core 경로를
가리킨다. 기존 package를 교체하거나 다시 만들지 않았다.

| 언어 | 실행 | Runner | Role 종료 판정 |
|---|---:|---|---|
| Java | 1회 | exit 0 | 네 role 모두 `ZLINK_FRAMEWORK_TERMINATION outcome=STOPPED reason=NONE`; `wait_status=143` 네 건, client 0, SIGKILL 없음 |
| .NET | 1회 | exit 0 | cleanup xtrace가 네 role 87240·87141·87068·86998에 `kill -9`; 정상 종료 아님 |
| Node | 1회 + 종료 확인용 strace 1회 | 두 번 exit 0 / PASS | strace 실행에서 role PID 9526이 SIGKILL; 정상 종료 아님 |

.NET runner `samples/TicTacToe/run_sample.sh:19-47`은 SIGINT 뒤 약 2초만 기다리고
남은 role을 SIGKILL하며 wait 실패를 무시한다. Node `samples/run-sample.mjs:455-465`도
SIGINT 뒤 500ms 후 SIGKILL하고 전체 PASS를 취소하지 않는다. 이 관찰만으로 두 언어가 같은
Core stack에 걸렸다고 단정할 수 없으며, 해당 runner 관찰 창을 늘려 추가 실행하지 않았다.
Node 확인용 strace는 `parity-node-exit.strace:605-640`에 SIGKILL과 `WTERMSIG==SIGKILL`을 기록했다.

C++ binding의 `Runtime/Sockets/socket.cpp:52-58`은 native close 뒤 completion owner를
정리하며, completion thread도 독립적으로 poller를 해제한다. Java Framework는
`ZLinkChannelSocketRegistry.java:1038-1075`에서 transport lock 안에서 monitor와 DEALER를
닫는다. Node는 `channel-socket-registry.ts:498-516`에서 readable poller를 정리한 뒤 monitor와
DEALER의 dispose를 함께 기다린다. .NET은 `ZLinkClientServerClientRuntime.cs:871-950`에서
control/monitor task와 monitor를 정리하고 socket을 dispose한다. 이런 호출 구조의 차이가
경쟁 노출 시점을 바꾸지만, 닫힌 등록 socket의 poller 해제로 Core executor를 재시작하는 것은
언어별 Framework가 보상할 책임이 아니다. .NET 파일은 병행 작업 중인 상태였으며 수정하지 않았다.

## 증거와 남은 항목

증거 root는 저장소 기준
`framework/languages/cpp/build/linux-ninja-c-e2e/diag-cleanup-137/evidence/`다.

- `current-{ttt,bingo}-{1,2,3}/`: 요구된 6회 실행의 `result.json`, `runner.log`, `runner.xtrace`,
  role maps, GDB stack, `artifacts/*/{logs,flow-logs}`.
- `current-ttt-state/`, `current-ttt-restart-2/`: 실패 lifecycle 값과 close 뒤 executor 시작 stack.
- `old-clean-{ttt,bingo}-1/`, `pre-monitor-{ttt,bingo}-{1,2,3}/`, `post-monitor-*`:
  같은 Framework 바이너리에서 Core만 교체한 비교.
- `public-close-race-*.log`, `public-close-race-state.txt`: binding 없는 공개 API 재현.
- `parity-{java,dotnet,node}/`, `parity-node-exit/`, `parity-node-exit.strace`: 언어별 결과와 종료 증거.
- `*-build.log`, `initial-sha256.txt`, `worktree-collision.txt`: build와 package provenance.

남은 실패는 Core close 회귀와 .NET·Node의 실제 role 정상 종료 미입증이다. 추가 debugger 실행
`current-ttt-restart`는 role 시작 전 기존 `mesh_node_vertical_test.cpp:734` assertion에서
실패했고, 다음 실행에서는 통과했다. 이 별도 preflight 실패의 source·assertion을 변경하지 않았다.
최초 공유 old build의 링크 오류/139와 격리 snapshot의 LICENSE symlink configure 오류는
유효한 비교 산출물로 대체했으며 판정에 사용하지 않았다.

변경 파일은 이 진단 문서 하나다. Main Core/package·C++ source/test·보호 spec 문서는
수정하지 않았다. 임시 GDB 계측은 해제했으며 진단 wrapper·GDB command 파일은 제거했다.
진단 로그와 비교 library, 공개 API 진단 binary는 build artifact로 보존한다.
