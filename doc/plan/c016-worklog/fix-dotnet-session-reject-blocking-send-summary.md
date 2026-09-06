# .NET STREAM session rejection shutdown 수렴 수정

## 결과

shutdown admission seal 뒤 새 STREAM session을 거부할 때 사용하던 blocking
`SendFlags.None` 경로를 제거했다. 거부 closing control과 기존 session의 ServerDrain closing
control은 이제 Framework의 awaitable async submit을 사용하고, session table은 별도 seal bool 대신
seal 시점의 shutdown cancellation token을 보관한다. 따라서 공유 STREAM lane에서 한 peer의 blocking
send가 다른 session의 closing과 heartbeat를 직렬로 막지 않으며, shutdown 취소가 진행 중인 거부
submit까지 도달한다.

ServerDrain closing submit의 `NotConnected`는 peer transport가 이미 닫힌 완료 상태로 처리하고 추가
disconnect를 하지 않는다. 다른 closing 실패는 처음 발생한 원래 `Exception` 인스턴스를 session에
보존하고, session table의 기존 `ZLinkDrainForceException(TeardownFailed, failures)`를 통해 drain
executor까지 전달한다. public API와 timeout, retry, timer는 추가하거나 변경하지 않았다.

- 소유 계층: Framework STREAM transport가 control의 DONTWAIT/async submit 방식을 소유하고,
  STREAM session runtime/table이 session teardown 결과와 원래 실패 원인을 소유한다.
- spec 조항: host relocation §14의 admission seal → accepted work drain → deadline → transport
  teardown 순서와 D-097/D-098의 bounded convergence를 적용했다.
- 교차언어 대조: Node `stream-session-runtime.ts:1361-1377` / `managed-stream.ts:153-179`와 Java
  `ZLinkStreamRuntime.java:1457-1490`는 closing을 awaitable non-blocking submit으로 처리한다. .NET의
  blocking `NONE`만 구조적 차이였으며 수정 뒤 같은 규칙이다.
- 변경 분류: **B — 기존 결함**. Binding/Core의 `Submit()` 계약을 상위에서 보상하지 않고 Framework가
  잘못 선택한 blocking 호출을 Framework 소유 경로에서 교정했다.
- 수정 전/후 규칙 수: closing control 제출 규칙 **2개**(late rejection=`NONE`, 나머지=
  DONTWAIT/async)에서 **1개**(awaitable DONTWAIT/async submit)로 줄였다. seal 여부와 취소 token도
  bool+별도 전달이 아니라 token 존재 여부 하나로 표현한다.

## 구현

- `Runtime/Streams/ZLinkSessionStreamTransport.cs:15-41`: managed stream과 fallback stream의 async
  submit 규칙을 한 owner에 둔다.
- `Runtime/Streams/ZLinkStreamFrameWriter.cs:5-29`: encoded control frame을 같은 async submit에
  전달하는 awaitable writer를 추가했다.
- `Runtime/Streams/ZLinkStreamSessionTable.cs:20-137,241-261`: seal token을 admission 상태로
  보관하고, late rejection을 `socket.SendAsync`로 제출하며, session별 원래 teardown failure를
  기존 drain exception으로 올린다.
- `Runtime/Streams/ZLinkStreamSessionRuntime.cs:433-513,1186-1192`: ServerDrain submit에 deadline
  token을 전달한다. `NotConnected`는 transport-closed 성공으로 끝내며 실제 실패는 첫 exception
  인스턴스를 보존한다.
- `Runtime/Streams/ZLinkStreamNodeRuntime.cs:104-111,527-638`: ingress cancellation을 session
  admission에 전달하고 seal/force-stop async 경로를 연결했다.
- `Runtime/Host/ZLinkFrameworkDrainExecutor.cs:48-58,94-124,459-475`,
  `Runtime/Host/ZLinkFrameworkRuntime.cs:444-460,1027-1037`와
  `Runtime/Host/ZLinkFrameworkRuntimeState.cs:153-163`: coordinator가 소유한 shutdown deadline token을
  최초 seal부터 STREAM session owner까지 전달한다. force-stop에서는 seal 직후 시작된 rejection이
  캡처한 orderly token을 active-session teardown보다 먼저 취소한 뒤 bounded force stop을 await한다.
- `tests/Zlink.Framework.UnitTests/Runtime/DrainCoordinatorTests.cs:1470-1557`: public
  `AddZLinkFramework`, `IZLinkFrameworkRuntime.ShutdownAsync`, STREAM connector만 사용하는 64/256
  session 회귀를 추가했다. 10초 orderly deadline의 절반인 5초 미만에 `Stopped/None`이어야 하며,
  sleep 없이 seal 상태와 accepted callback barrier로 순서를 고정한다.
- `tests/Zlink.Framework.UnitTests/Runtime/StreamSessionForcedCleanupTests.cs:1275-1395`: blocking sync
  send가 호출되지 않고 async submit이 seal token으로 취소되는지, `NotConnected` 뒤 disconnect를
  다시 하지 않는지, 실제 failure의 동일 exception 인스턴스가 `TeardownFailed` 원인으로 남는지
  검증한다.

단순히 `TrySubmit`만 호출하는 대안은 completion/cancellation과 원인 전달을 잃으므로 선택하지
않았다. 별도 timeout/retry로 blocking 시간을 가리는 대안도 §14의 owner/deadline 규칙을 늘리므로
배제했다.

## 검증

모든 .NET gate 전 `/proc/loadavg < 10`, `pgrep -c lto1 == 0`을 확인했고 지정된 lock과
`perf/scripts/dotnet-env.sh`를 사용했다.

| 검증 | 결과 |
|---|---|
| 집중 `StreamSessionForcedCleanupTests|DrainCoordinatorTests` | **86/86 통과**, 64/256 public 회귀와 seal deadline 선취소 포함 |
| unit split `FullyQualifiedName!~CanonicalActorJoinIngressReplyTests` | **2009/2009 통과**, 3분 6초 |
| unit complement `FullyQualifiedName~CanonicalActorJoinIngressReplyTests` | **16/16 통과**, 22초 |
| `Zlink.Framework.SampleRegressionTests` | **157/157 통과**, 402 ms |
| `samples/run_samples.sh` | **7/7 통과**, ZoneWorld 전체 relocation/maintenance 포함 |
| perf Client/SessionServer/ChannelServer Release build | **통과**, 오류 0 |
| `git diff --check` | **통과** |

Release build에는 기존 `Runtime/Spots/ZLinkSpotNodeCatalog.cs:768`의 CS8619 nullability warning
1건만 남았다.

## 64-connection SIGTERM 재현

진단의 read-only harness
`/dev/shm/zlink-perf-dotnet/diag-followup-20260906/repro-tools/zlink-perf-followup-eventpipe-stacks.py`를
수정 없이 사용했다. 세 role의 Release DLL을 먼저 갱신하고 새 output root
`/dev/shm/zlink-perf-dotnet/fix-session-reject-20260906-final3`에서 session-echo-only,
64 connections, payload 4096, warmup 1초, 측정 2초를 3회 실행했다.

| run | harness | SIGTERM → server exit | exit | host termination |
|---|---|---:|---:|---|
| eventpipe-stacks-session-1 | valid | **0.428 s** | 0 | Stopped / None |
| eventpipe-stacks-session-2 | valid | **0.276 s** | 0 | Stopped / None |
| eventpipe-stacks-session-3 | valid | **0.327 s** | 0 | Stopped / None |

세 run 모두 외부 75초 관찰 cap이나 Framework orderly deadline에 닿지 않았고 SIGKILL 없이
자체 종료했다. 확대 검증은 unit public contract에서 256 sessions까지 실행해 통과했으므로 별도
512 perf run은 요청 범위에 추가하지 않았다.

## 변경 경계와 남은 실패

변경은 위의 .NET Framework source 8개, unit test 3개와 이 요약 파일뿐이다. Core, bindings,
다른 언어, 보호된 spec/framework 문서, perf source, `ZLinkManagedMeshNode.cs`,
`MeshNodeShutdownSealTests.cs`는 수정하지 않았고 commit하지 않았다.

최종 unit split, STREAM 집중 suite, public 64/256 회귀, complement, SampleRegression, samples와
perf 재현에 남은 test failure는 없다.
