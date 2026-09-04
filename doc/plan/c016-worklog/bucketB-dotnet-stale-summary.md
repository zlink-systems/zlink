# bucket B — .NET stale-authority reply stall 조사

## 결론

분류는 **Core 결함**이다. Framework가 stale-authority terminal reply를 잘못 만들거나 .NET
binding이 completion을 버린 것이 아니다. 두 ROUTER가 서로 연결하는 mesh에서 caller가 먼저
시작하여 아직 bind되지 않은 owner에게 연결하면 물리 transport pair가 둘 생긴다. 이 상태에서
Core는 request를 받은 pair와 다른, 같은 logical RID의 현재 Completion pipe로 reply를 보낸다.
caller Core는 reply를 실제로 읽지만 pending request에 저장한 submit-time pair와 다르다는 이유로
조용히 폐기한다. Pending request는 그대로 남아 3초 뒤 `ZLINK_REQUEST_TIMED_OUT(101)`로 끝난다.

정확한 Framework 시작 순서를 옮긴 순수 C 재현이 5회 모두 같은 `101`을 반환했다. 작업 지시의
“failing pure-C repro = Core bug → STOP”에 따라 Framework와 .NET binding에는 수정이나 회귀
테스트를 추가하지 않았다.

## 원인 경로

테스트는 두 peer를 모두 등록한 다음 caller, owner 순서로 시작한다
(`framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/StatefulServiceRuntimeTests.cs:892-903`).
`ZLinkManagedMeshNode.Start()`는 자기 ROUTER를 bind한 뒤 미리 등록한 peer에 연결하므로
(`framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:288-345`),
caller의 owner 연결은 owner socket이 생기기 전에 pending 상태가 된다. owner가 시작하면 자기
ROUTER를 bind하고 caller로 역방향 연결한다. Framework의 duplicate-admission 처리는 한 logical
peer만 남기지만 두 Core transport는 유지한다. 실제 trace에도 owner의
`mesh_peer_duplicate_retire`와 `mesh_peer_duplicate_retire_skip_transport`가 연속으로 기록됐다.
나머지 두 실패 테스트도 같은 peer 등록 및 caller-first 시작 순서를 사용한다
(`StatefulServiceRuntimeTests.cs:997-1002`, `:1088-1093`).

Framework의 request와 reply key는 맞다.

- Caller는 대상 actor 요청을 대상 node RID로 제출한다
  (`ZLinkManagedMeshNode.cs:9413-9442`).
- Owner는 stale authority를 확인한 뒤 같은 native `ReplyOperation`으로
  `Conflict/ActorLocationStale`을 보낸다 (`ZLinkManagedMeshNode.cs:7168-7190`).
- `SubmitOrQueueNativeReply()`는 Core의 첫 terminal 제출 결과를 그대로 사용한다
  (`ZLinkManagedMeshNode.cs:10236-10257`). 이 제출은 `Ok`였다.

reply가 사라지는 지점은 Core 내부의 transport pair 검증이다.

1. ROUTER peer의 reply는 Completion pipe를 사용한다. 받은 request의 Application pipe를 다시
   사용할 수 있으려면 captured pipe가 현재 route token을 유지해야 한다
   (`core/src/api/socket/socket_request_reply_runtime_io.cpp:1334-1378`). 재현에서는 captured
   `router_reply_target_t`의 Application pair와 generation은 유효했지만
   `route_binding_token=0`이었다. 따라서 `target_.route_binding_token != 0` 조건을 통과하지
   못했다.
2. Core는 같은 logical RID의 현재 Completion pipe를 대신 고른다
   (`socket_request_reply_runtime_io.cpp:1380-1406`). 이는
   `core/doc/spec/core/socket/07-router.ko.md:285-293` 및 D-072의 current logical-RID 규칙과
   일치한다.
3. `send_public_router_reply_with_wait()`는 그 다른 Completion pipe에 frame을 쓴 뒤 성공을
   반환한다 (`core/src/api/socket/socket_request_reply_submit_api.cpp:459-466`). 그래서 owner의
   `zlink_reply_part()`와 Framework submission은 모두 `Ok`다.
4. Caller Core는 frame을 Completion pipe에서 읽고 그 pipe의 pair ID와 generation을
   `complete_reply_from_transport()`에 넘긴다
   (`core/src/api/socket/socket_request_reply_dispatch.cpp:269-297`).
5. Pending request에는 request를 제출한 Application pair가 저장돼 있다.
   `take_pending_reply_from_transport_locked()`는 incoming Completion pipe가 그 submit-time
   pair와 정확히 같아야 한다고 검사한다
   (`core/src/api/socket/socket_request_reply_pending_api.cpp:105-145`). 다른 current pair로 온
   reply는 여기서 `false`가 된다.
6. 호출자는 이 불일치를 protocol error로 처리하지 않는다. Reply payload를 닫고
   `completion_message_accepted`를 반환한다
   (`core/src/api/socket/socket_request_reply_dispatch.cpp:151-160`). Pending request는 제거되지
   않아 deadline에서 `101` completion을 만든다.

GDB로 같은 순수 C 재현의 두 경계를 직접 확인했다. Pair ID는 실행마다 임의로 정해지므로 다음
값은 각 실행의 표본이다.

- Owner의 captured target: Application pair `1064892148662593035`, generation `1`,
  `route_binding_token=0`.
- `retain_reply_transport_pipe()`의 반환값: 다른 Completion pair
  `12836427960345874753`, generation `1`.
- Caller의 reply source: Completion pair `9740423993356805444`, generation `1`, locally
  initiated.
- 같은 request의 pending/correlation: Application pair `834701839219104314`, generation `1`,
  not locally initiated.

따라서 send 쪽은 logical RID의 현재 route를 따르지만 receive 쪽은 과거의 물리 pair에 고정된다.
두 규칙이 같은 Core request/reply 흐름 안에서 충돌한다.

## 분류

| 범위 | 판정 | 근거 |
|---|---|---|
| Framework | 원인 아님 | 올바른 target RID와 captured reply operation을 사용했고 terminal 제출도 `Ok`였다. |
| .NET binding | 원인 아님 | Reply completion은 public queue에 없었고, timeout completion은 원래 ID/UserContext로 정상 drain됐다. |
| Core | 원인 | 순수 C에서 재현됐고 Core가 current pair의 reply를 읽은 뒤 submit-time pair 비교에서 폐기했다. |
| Test | 원인 아님 | 동일 socket을 유지하는 정상 reciprocal ROUTER 구성이고, 기대값은 logical-RID reply 계약과 맞다. |

## .NET trace 근거

기존 Framework file sink와 `/tmp`의 native forwarding shim으로 첫 테스트와 같은 본문을 실행했다.
Shim은 `poller_add`, `poller_wait`, `zlink_request_part`, `zlink_router_recv_part`,
`zlink_reply_part`, `zlink_completion_recv`만 기록했으며 Core build나 package를 바꾸지 않았다.

Native trace `/tmp/zlink-stale-native.WwGipN.log`의 순서는 다음과 같다.

```text
5975.504 caller poller_add events=0x27
5975.515 owner  poller_add events=0x27
5975.745 caller request_part FINAL timeout=2997 context=0x792077351240 result=0 completion=1
5975.746 owner  router_recv token=1 more=1
5975.746 owner  router_recv token=1 more=0
5975.754 owner  reply_part token=1 FINAL result=0
5978.746 caller completion_recv kind=2 id=1 context=0x792077351240 request_result=101 parts=0
```

Reply 제출 후 약 3초 동안 caller에는 `POLLCOMPLETION` event나 request completion이 없었다.
Deadline에서만 `POLLCOMPLETION`이 발생했고, `zlink_completion_recv()`가 request 제출 때의 ID `1`과
정확히 같은 UserContext로 `101`을 반환했다. Framework trace
`/tmp/zlink-stale-fw.pyF9fL.log`도 `actor_stale_path`와
`native_terminal_reply_submit ... result=Ok` 뒤 3초 후
`managed_operation_completion_rejected ... result=TimedOut ... present_key=False`만 기록했다.

.NET binding은 completion을 잃지 않았다. Poller는 socket을
`POLLIN|POLLERR|POLLOUT|POLLCOMPLETION`으로 등록하고 native event가 오면 sole owner를 drain한다
(`bindings/dotnet/src/Zlink/Runtime/Eventing/Poller.cs:229-263`). `CompletionOwner`는 request 제출
전에 UserContext entry를 등록하고 같은 pointer를 FINAL에 넘긴다
(`bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:149-169`,
`:461-486`). Native completion이 존재하면 `zlink_completion_recv()` 후 그 UserContext로 entry를
찾는다 (`CompletionOwner.cs:286-319`). 이번에는 reply completion 자체가 Core public completion
queue에 생기지 않았으며, 나중의 timeout completion은 정확한 context로 정상 drain됐다.

## D-068·D-072와의 관계

이 결함은 D-068/D-072가 다룬 handover 테스트와 같은 테스트가 아니다. 그 테스트는 기존 DEALER로
request를 보낸 뒤 같은 RID의 새 DEALER socket을 만들어 source socket 자체를 교체한다
(`doc/plan/c016-worklog/decisions.ko.md:463-481`). 이번 테스트는 caller ROUTER socket 하나를 계속
사용하며 handover API나 source 교체가 없다.

공유하는 부분은 Core가 reply를 captured 물리 connection이 아니라 logical RID의 current route로
보낸다는 점뿐이다. D-072는 이 송신 규칙을 유지하라고 판정했다
(`decisions.ko.md:522-533`). 그러므로 Core 수정은 reply를 과거 pair로 되돌리는 방식이 아니라,
같은 requester socket의 current logical-RID Completion pipe에서 받은 reply가 해당 pending
request를 완료할 수 있도록 수신 correlation을 일치시키는 방향이어야 한다. 현재
`socket_request_reply_pending_api.cpp:105-116`의 submit-time pair 고정은 이 판정과 충돌한다.

## 순수 C 재현

재현 소스는 `/tmp/repro_rr_framework.cpp`에 보존했다. 다음 조건을 Framework와 같게 맞췄다.

- ROUTER 두 개, explicit RID와 reciprocal connect
- `Mandatory`, `RID_DUPLICATE_HANDOVER`, `Linger=0`, `MaxMessageSize=-1`, 양쪽 HWM
- socket 생성 직후 `POLLIN|POLLERR|POLLOUT|POLLCOMPLETION` poller 등록
- caller bind/connect를 먼저 수행하고, 그 뒤 owner socket 생성/bind/역방향 connect
- 양방향 Hello 형태 DATA와 owner Admit 형태 DATA를 교환한 뒤 multipart request 제출
- `DONTWAIT`, completion ID/UserContext, 3초 request timeout
- owner의 별도 thread가 받은 token으로 50 ms 뒤 reply

빌드와 실행 명령은 다음과 같다. Core와 package는 다시 빌드하지 않았다.

```bash
g++ -std=c++17 -I core/include /tmp/repro_rr_framework.cpp \
  -L core/build-dev/lib -lzlink \
  -Wl,-rpath,/home/hep7/project/zlink/core/build-dev/lib \
  -pthread -o /tmp/repro_rr_framework

for run in 1 2 3 4 5; do
  /tmp/repro_rr_framework
  status=$?
  printf 'run=%s rc=%s\n' "$run" "$status"
done
```

결과는 5/5 동일했다. `request result=0`, `reply result=0` 뒤
`completion kind=2 id=1 context_match=1 result=101 parts=0`, process exit `7`이었다. 두 socket을
모두 bind한 다음 connect하는 대조군은 같은 옵션·poller·admission traffic·threaded reply 조건에서
25/25 `ZLINK_REQUEST_OK`였다. 실패 조건은 caller의 선행 pending connect와 이후 reciprocal
connection 순서다.

GDB 확인은 build-dev shared library의 기존 debug symbol을 사용했다. 대표 명령은 다음과 같다.

```bash
gdb -q -batch \
  -ex 'set pagination off' \
  -ex 'set debuginfod enabled off' \
  -ex 'start' \
  -ex 'rbreak retain_reply_transport_pipe' \
  -ex 'continue' \
  -ex 'p target_' \
  -ex 'finish' \
  -ex 'p ((zlink::pipe_t*)$rax)->_transport_pair_id' \
  --args /tmp/repro_rr_framework

gdb -q -batch \
  -ex 'set pagination off' \
  -ex 'set debuginfod enabled off' \
  -ex 'start' \
  -ex 'break /home/hep7/project/zlink/core/src/api/socket/socket_request_reply_pending_api.cpp:95' \
  -ex 'continue' \
  -ex 'p transport_pair_id_' \
  -ex 'p state_->pending_requests._live_head->second.transport_pair_id' \
  --args /tmp/repro_rr_framework
```

## 변경과 회귀 테스트

Production source는 수정하지 않았다. 조사 시작 때 있던 임시 file sink와 추가 trace는 모두
제거했다. 다음 두 파일의 `git diff`는 비어 있다.

- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Diagnostics/ZLinkFrameworkDebugLog.cs`
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs`

Core 소유 결함이므로 Framework 또는 binding 회귀 테스트를 추가하면 잘못된 계층에서 문제를
고정한다. Core 회귀 테스트는 `/tmp/repro_rr_framework.cpp`의 시작 순서와 reciprocal ROUTER
topology를 옮겨야 한다. Reply가 current logical-RID Completion pipe로 들어와도 원래 completion
ID/UserContext가 `ZLINK_REQUEST_OK`와 payload 하나로 정확히 한 번 완료되는지 검사해야 한다.

## 실행 결과

요청받은 dotnet 명령은 그대로 실행을 시도했지만 다른 supervisor 작업이
`/tmp/zlink-dotnet-gate.lock`을 보유하고 있었다. 3분 넘게 test process가 시작되지 않아 이 작업의
lock waiter만 중단했다. 조사 종료 시에도 PID 20027의 전체 unit gate가 lock을 보유했고, PID
29028과 40020도 같은 focused test를 기다리고 있었다.

```bash
export TMPDIR=/dev/shm/zlink-tmp-dotnet \
       ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib \
       UseSharedCompilation=false MSBUILDDISABLENODEREUSE=1 \
       DOTNET_CLI_TELEMETRY_OPTOUT=1
pkg_hash=$(sha256sum .artifacts/wsl/nuget/Systems.Zlink.0.17.0.nupkg | awk '{print $1}')
export NUGET_PACKAGES=/dev/shm/zlink-tmp-dotnet/nuget-${pkg_hash:0:16}
cd framework/languages/dotnet
flock -w7200 /tmp/zlink-dotnet-gate.lock dotnet test tests/Zlink.Framework.UnitTests \
  --filter 'FullyQualifiedName~StatefulServiceRuntimeTests.RemoteActorStaleAuthority'
```

결과는 **test 미실행(lock 대기 중 자체 waiter 중단)**이다. Lock을 우회하여 concurrent dotnet
test를 실행하지 않았다.

Lock과 xUnit console capture를 피한 진단에만, 첫 테스트의 setup과 request 본문을 그대로 옮긴
임시 executable을 사용했다.

```bash
ZLINK_LIBRARY_PATH=/tmp/zlink-native-shim/libzlink.so \
ZLINK_NATIVE_TRACE_FILE=/tmp/zlink-stale-native.WwGipN.log \
ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1 \
ZLINK_DEBUG_FRAMEWORK_LOG_FILE=/tmp/zlink-stale-fw.pyF9fL.log \
dotnet /tmp/zlink-stale-diag/Zlink.Framework.UnitTests.dll
```

결과는 `terminal=101 failure=0`으로 재현됐다. 이는 test gate 결과가 아니라 trace 수집용
진단 실행이다.

요청된 “세 테스트 3회 green”과 `StatefulServiceRuntimeTests` 1회는 실행하지 않았다. Core가 아직
같은 reply를 폐기하므로 green 검증을 보고할 수 없고, failing pure-C repro에서 중단하라는 지시를
따랐다.

## BLOCKERS

- Core request/reply 소유 모듈을 수정해야 한다. D-072의 logical-RID reply 규칙을 유지하면서
  requester의 pending correlation이 current same-RID Completion pipe를 받아들이도록
  `socket_request_reply_pending_api.cpp:89-158`의 물리 pair 검증을 정렬해야 한다. 단순히 모든
  pair를 허용하지 말고 pending logical RID, current route, request identity, connection lifecycle을
  함께 검증해야 한다.
- Core 회귀 테스트와 fresh local package가 준비된 뒤 세 stale-authority 테스트를 3회, 이어서
  `FullyQualifiedName~StatefulServiceRuntimeTests`를 한 번 실행해야 한다.
- 조사 시점에는 `/tmp/zlink-dotnet-gate.lock`을 다른 전체 unit run이 장시간 보유하고 있어 요청된
  dotnet 검증을 시작할 수 없었다.
- Spec 변경은 필요하지 않다. `07-router.ko.md:285-293`과 `:311-316`은 이미 current logical RID,
  physical generation이 token을 무효화하지 않는다는 계약을 명시한다. 보호된 spec 경로는 읽기만
  했고 수정하지 않았다.
