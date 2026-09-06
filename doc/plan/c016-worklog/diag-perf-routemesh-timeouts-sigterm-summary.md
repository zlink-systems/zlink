# .NET perf 후속 진단 — RouteMesh timeout과 SIGTERM 종료

감독 검토용 진단이다. **A의 원인은 미확정이다. B에서는 Session shutdown의 Framework 결함을 확인했다.** Framework·Core·binding·spec을 수정하지 않았고 commit하지 않았다. 제품 하네스 수정도 없다. 저장소 변경은 이 보고서 하나다.

## 판정

| 대상 | 관찰과 판정 |
|---|---|
| A: RouteMesh 4096 timeout ×16 | 시작 직전 load < 5인 30초 실행 5회, `yes` 20개를 추가한 30초 실행 3회, 실제 시스템 load 56–61의 30초 실행 1회 모두 warmup/measured 오류 0. **재현 실패이며 load-sensitivity로 확정하지 않는다.** |
| B: Session SIGTERM > 5초 | 64 connections에서 **5.812초** 종료를 재현했다. 최종 결과는 `Stopped / ForceStopped / TeardownFailed`. Session 거부 경로의 blocking send가 공유 STREAM queue를 1000 ms씩 점유한다. 512 connections 확대 재현에서는 **75초 뒤에도 종료하지 않아 SIGKILL**했다. |
| B: RouteMesh SIGTERM | 단독 target 3회와 echo 후 source/target 3쌍은 정상 종료했다. 원래 PID 91811의 종료 실패는 재현하지 못했다. Session 원인을 RouteMesh에 일반화하지 않는다. |
| 하네스 | ASP.NET Core host가 SIGTERM을 받아 Framework `StopAsync`를 기다린다. HTTP/계측 thread 잔류가 원인이라는 증거는 없다. cleanup의 5초를 늘리거나 종료 순서를 바꾸어 Framework 결함을 가리지 않았다. |

## 실행 환경과 해시

- `main`에서 조사했으며 기존 사용자 변경을 보존했다. 다른 작업의 ClientServer readiness 수정 파일을 포함해 `src/`는 읽기 전용으로 취급했다.
- 지정된 `dotnet-env.sh`를 사용했다. 빌드는 `/tmp/zlink-dotnet-gate.lock`, perf 실행은 `/tmp/zlink-samples-gate.lock`으로 보호했다. 공유 잠금 대기는 관찰 시간에 포함하지 않았다.
- A는 **4096 logical bytes, 16 streams × inflight 1, warmup 5초 + measured 30초**다. request/socket send timeout은 1000 ms, settle 5000 ms, setup 30000 ms 그대로다. 모든 baseline은 manual 구성으로 Store를 사용하지 않는다.
- “quiet”는 사용자가 요구한 **실행 시작 직전 `/proc/loadavg` 첫 값 < 5**를 뜻한다. 실행 중에는 perf 자체와 다른 작업 때문에 load가 올라갔다. 전체 30초 동안 다른 모든 작업이 없었다고 주장하지 않는다.
- 다른 작업의 build/commit과 공존해 관리 DLL 해시는 두 종류다. **quiet-3 이후와 세 인공 부하 대조군은 같은 F2**다. 원래 phase 1 DLL 해시와도 다르므로 원래 작업 당시의 모든 working-tree/스케줄링 조건을 복원한 실험은 아니다.

| 태그 | 파일 | SHA-256 |
|---|---|---|
| N0 | `core/build-dev/lib/libzlink.so.0.17.0` 및 실제 로드한 package native payload | `64567f1715b3f1527afbc1c290e2b262d02d722768e160227a6f9815bdd4bb43` |
| F1 | quiet-1, quiet-2의 `Zlink.Framework.dll` | `e59db6ac2bdc7e50decec4fd9e8ccc3389db591699480ca18c2d2e4606fdad49` |
| F2 | quiet-3 이후의 `Zlink.Framework.dll` | `7b129dca8708091f91dc44b02667c494ec2b3fa5ffcea01bbdb14d6c331ac464` |

실제 native 로드 경로는 각 role의 `bin/Release/net8.0/runtimes/linux-x64/native/libzlink.so`이며 N0와 일치한다. 각 성공 셀의 `loaded-artifacts.json`, A의 `*-observation.json`, `env.json`, `source-provenance.json`에 근거를 보존했다. `ZLINK_LIBRARY_PATH`의 디렉터리를 먼저 열다 발생한 loader 예외는 package 경로로 fallback한 초기 예외다. 로드 실패나 다른 Core 사용으로 해석하지 않는다.

**D-118 적용 Core로 시험한 결과가 아니다.** 소스의 `core/src/runtime/core/pipe.cpp:1874`에는 수정이 있지만 `core/build-dev`를 재빌드하지 않았다. D-118 수정의 검증 또는 새 release baseline으로 이 결과를 채택하면 안 된다.

원본·trace·재현 실행기는 [진단 artifact root](/dev/shm/zlink-perf-dotnet/diag-followup-20260906)에 있다. 각 표의 run 이름이 그 아래 디렉터리 이름이다. 모든 결과를 보존했으며 기존 phase 1 원본은 덮어쓰지 않았다.

## A. 재현 결과

`W sent/timeout`은 warmup, `M sent/completed/settle`은 measured cohort의 합이다. 모든 새 실행의 cancelled/unresolved도 0이다. KOPS는 비교 조건에서의 관찰 수치이며 release 성능 판정이 아니다.

| Run | load 시작 / 최대 | W sent / timeout | M sent / completed / settle | M timeout | KOPS | 해시 |
|---|---:|---:|---:|---:|---:|---|
| quiet-1 | 0.35 / 6.60 | 66478 / 0 | 489361 / 489350 / 11 | 0 | 16.3117 | N0/F1 |
| quiet-2 | 4.06 / 7.26 | 67727 / 0 | 493193 / 493177 / 16 | 0 | 16.4392 | N0/F1 |
| quiet-3 | 4.77 / 9.71 | 68127 / 0 | 478154 / 478138 / 16 | 0 | 15.9379 | N0/F2 |
| quiet-4 | 4.50 / 6.88 | 67214 / 0 | 484062 / 484046 / 16 | 0 | 16.1349 | N0/F2 |
| quiet-5 | 0.76 / 7.04 | 60261 / 0 | 492354 / 492338 / 16 | 0 | 16.4113 | N0/F2 |
| load-1 | 7.04 / 16.26 | 12766 / 0 | 101299 / 101283 / 16 | 0 | 3.3761 | N0/F2 |
| load-2 | 16.26 / 20.98 | 12827 / 0 | 99532 / 99516 / 16 | 0 | 3.3172 | N0/F2 |
| load-3 | 20.98 / 22.16 | 12624 / 0 | 98453 / 98437 / 16 | 0 | 3.2812 | N0/F2 |
| ambient-1 | 56.44 / 61.47 | 2253 / 0 | 76000 / 75984 / 16 | 0 | 2.5328 | N0/F2 |

`load-1..3`은 정확히 소유한 `yes > /dev/null` PID 20개만 추가·종료했다. 부하 대조군은 처리량이 3.28–3.38 KOPS로 줄었지만 timeout은 발생하지 않았다. `ambient-1`은 인공 부하를 추가하지 않았으며 실제 다른 작업과 경쟁했다. 최대 load 61.47에서도 오류 0이었다. 따라서 “부하가 높아서 1초 timeout을 넘었다”는 설명은 입증되지 않았다.

### 기존 증거의 phase 정정

- `phase1-smoke/channel-echo-only/4096/*routemesh*/server-channel-0.json`은 **resetSeq=1, measured 2초**다. sent=11904, completed=11888, timeout=16, settle 성공=0이다. 요청문과 이전 보고서의 “smoke warmup 실패” 표기는 raw와 다르다.
- `phase1-full-routemesh-4096/.../server-channel-0.json`은 **resetSeq=0, warmup 5초**다. sent=59126, completed=59110, timeout=16이다. 이 실행에서 measured reset/barrier는 열리지 않았다.
- 이전 Normal tracing 2초·30초 실행의 source/target flow 파일을 먼저 읽었다. 두 실행에는 오류가 없었다. tracing 결과를 일반 baseline으로 대체하지 않았다.

### 원인 후보별 확인

| 후보 | 근거와 남은 판단 |
|---|---|
| 4096에만 다른 timeout | `perf/scripts/runner.py:315`의 workload 생성은 두 payload 모두 request 1000 ms. `ServerApplication.cs:52`, `ChannelEchoOnlyScenario.cs:60`이 그 값을 public call에 그대로 적용한다. payload별 분기가 없다. |
| 과도한 pipeline | `ChannelEchoOnlyScenario.cs:48`의 loop는 16 × 1이다. 각 loop가 public request terminal을 기다린 뒤 다음 호출을 한다. 추가 retry/별도 completion pump가 없다. |
| warmup/reset race | `Measurement.cs:75`, `:140`의 drained 조건과 `runner.py:414` 이후 barrier를 확인했다. 원래 30초 실패는 resetSeq=0이므로 measured reset과의 경쟁이 원인이라는 설명은 맞지 않는다. |
| source application counter lock | `Measurement.cs:101`, `:109`의 public status 조회는 counter lock 밖이다. 해당 contract test도 통과했다. 실패 raw의 source/target 상태 표본 최대 간격은 smoke 107.331/104.670 ms, full warmup 116.071/102.580 ms다. 프로세스 전체가 1초 정지했다는 증거가 없다. 특정 transport/실행 queue의 정지는 이 표본으로 배제할 수 없다. |
| HWM/credit | 실패 당시 host는 Serving/Ready, pressure Running, capacityWaitCount=0, Core blockedRatioPpm=0. full warmup target peak queue bytes=104561, totalAppliedHwmBytes=2097152. 포화 원인을 확정할 근거가 없다. 표본으로 순간적인 credit 문제까지 배제한 것은 아니다. |
| D-118 | D-118은 HWM=1에서 READY 뒤 빈 pipe의 첫 DATA admission/writable 진행 문제다. 이 셀은 typed probe와 수만 건 echo가 성공한 뒤 실패했다. 동일 원인으로 연결할 공개 재현이 없고 N0는 수정 전 library이므로 관계는 미확정이다. |
| reply route/admission | raw에 NotConnected/Unavailable/ProtocolError가 아닌 TimedOut만 있다. `ZLinkSpotOutboundTransport.cs:230` → `ZLinkRequestFailureMapper.cs:92`의 native timeout 투영이다. `Measurement.cs:203`은 그 public kind를 집계한다. 하네스가 timeout을 새로 만든 것이 아니다. 소실 transition은 실패 flow가 없어 미확정이다. |

위 표에서 짧게 쓴 perf 파일은 `framework/languages/dotnet/perf/` 아래이며, Runtime 파일은 `framework/languages/dotnet/src/Zlink.Framework/Runtime/` 아래다. 후보 검토만으로 A를 Core/Framework의 특정 결함으로 확정하거나 timeout을 바꾸지 않았다.

## B. SIGTERM 재현 결과

아래 모든 실행은 N0/F2다. idle은 workload 없는 단독 서버다. postwork/EventPipe는 64 connections(Session) 또는 16 streams(RouteMesh), 4096 bytes, warmup 1초 + measured 2초 뒤 종료다. 두 512-connection 실행은 **bounded teardown을 확인하기 위한 확대 재현**이며 원래 64-connection 셀과 합산하지 않는다. 모든 application timeout은 유지했다.

`Stopped`/`ForceStopped`는 termination outcome이다. 정상 반환한 모든 host의 state는 `Stopped`였다. 종료 시간은 SIGTERM 발송부터 process exit까지의 관찰 값으로 약 25 ms polling 오차가 있다. idle의 최종 상태는 로그, 전송 전 상태는 `/perf/stats`로 확인했다.

| Run / server | connections | 시작 load | SIGTERM→exit 초 | 최종 outcome / reason | exit |
|---|---:|---:|---:|---|---:|
| sigterm-idle-session-1 | 0 | 1.54 | 0.233 | Stopped / None | 0 |
| sigterm-idle-session-2 | 0 | 1.54 | 0.259 | Stopped / None | 0 |
| sigterm-idle-session-3 | 0 | 1.54 | 0.234 | Stopped / None | 0 |
| sigterm-idle-channel-1 | 0 | 1.54 | 0.208 | Stopped / None | 0 |
| sigterm-idle-channel-2 | 0 | 1.54 | 0.233 | Stopped / None | 0 |
| sigterm-idle-channel-3 | 0 | 1.50 | 0.232 | Stopped / None | 0 |
| postwork-session-1 / session-0 | 64 | 22.16 | 0.277 | ForceStopped / TeardownFailed | 0 |
| postwork-session-2 / session-0 | 64 | 20.54 | 0.979 | Stopped / None | 0 |
| postwork-session-3 / session-0 | 64 | 17.76 | 0.226 | ForceStopped / TeardownFailed | 0 |
| postwork-channel-1 / channel-0 | 0 | 17.76 | 0.201 | Stopped / None | 0 |
| postwork-channel-1 / channel-1 | 0 | 17.76 | 0.201 | Stopped / None | 0 |
| postwork-channel-2 / channel-0 | 0 | 16.42 | 0.201 | Stopped / None | 0 |
| postwork-channel-2 / channel-1 | 0 | 16.42 | 0.201 | Stopped / None | 0 |
| postwork-channel-3 / channel-0 | 0 | 15.42 | 0.226 | Stopped / None | 0 |
| postwork-channel-3 / channel-1 | 0 | 15.42 | 0.226 | Stopped / None | 0 |
| eventpipe-session-1 / session-0 | 64 | 55.75 | 0.452 | Stopped / None | 0 |
| eventpipe-session-2 / session-0 | 64 | 49.24 | 5.812 | ForceStopped / TeardownFailed | 0 |
| eventpipe-session-3 / session-0 | 64 | 41.67 | 1.305 | ForceStopped / TeardownFailed | 0 |
| eventpipe-stacks-session-1 / session-0 | 64 | 2.65 | 0.377 | ForceStopped / TeardownFailed | 0 |
| eventpipe-stacks-session-2 / session-0 | 64 | 2.51 | 0.427 | ForceStopped / TeardownFailed | 0 |
| eventpipe-stacks-session-3 / session-0 | 64 | 2.47 | 1.406 | ForceStopped / TeardownFailed | 0 |
| bounded-512-session-1 / session-0 | 512 | 11.31 | >75 (SIGKILL) | 미완료; force_stop_begin | -9 |
| bounded-512-session-2 / session-0 | 512 | 11.59 | 0.531 | ForceStopped / TeardownFailed | 0 |

RouteMesh의 connections=0은 STREAM connector가 없다는 뜻이다. postwork Channel은 source/target peer 연결과 16 logical streams를 사용했다. A의 일반 실행 9개 셀에서도 표준 cleanup에 SIGKILL은 없었지만, 그 cleanup은 process별 순차 wait이므로 각 PID의 SIGTERM 기준 종료 시간을 역산하지 않았다.

### 1. 하네스는 Framework shutdown을 호출하고 기다린다

공개 사용 경로는 다음과 같다.

- `perf/ZLink.Framework.Perf.ServerSupport/ServerApplication.cs:36`의 `WebApplication.CreateBuilder`와 `:49`의 `AddZLinkFramework`가 ASP.NET Core lifetime/host 통합을 사용한다.
- `perf/ZLink.Framework.Perf.SessionServer/Program.cs:10`은 `app.RunAsync()`, `perf/ZLink.Framework.Perf.ChannelServer/Program.cs:33`은 `app.WaitForShutdownAsync()`를 기다린다.
- `src/Zlink.Framework.AspNetCore/ZLinkFrameworkServiceRegistrar.cs:168`이 hosted service를 등록한다. `ZLinkFrameworkHostedService.cs:11` → `Runtime/Host/ZLinkFrameworkHostRuntimeCoordinator.cs:51` → `maintenance.ShutdownAsync(...)`를 await한다.
- `ServerApplication.cs:73`의 HTTP endpoint와 `Measurement.cs:96`의 sampler task가 별도 foreground lifecycle을 만들지는 않는다. 실패 재현에서도 로그의 `Hosting stopping`과 `shutdown_step`까지 들어갔다. **신호 미처리나 shutdown 호출 누락이 아니다.**

`runner.py:189`의 cleanup은 SIGTERM 뒤 5초 wait 후 SIGKILL한다. 반면 Framework 기본 orderly deadline은 `ZLinkFrameworkMaintenanceRuntime.cs:15`의 30초다. 이번 64-connection 5.812초 사례는 이 5초 경계를 넘지만 Framework deadline 안에는 반환했다. 이 숫자만으로 bounded teardown 위반이라고 판정하지 않는다.

### 2. Framework 원인: session 거부의 blocking send가 공유 queue를 점유한다

아래 경로는 source와 CLR exception stack 양쪽에서 확인했다.

1. **원인 호출** — `Runtime/Streams/ZLinkStreamSessionTable.cs:39`, `:196–212`: shutdown admission seal 뒤 `GetOrCreateAsync`의 거부 경로가 `RejectNewSession`을 실행한다. `:202`에서 session-closing control을 **`SendFlags.None`**으로 보내고, 예외를 삼킨 뒤 peer disconnect를 시도한다. 어느 ingress 사건(지연된 Ready 또는 control packet)이 원래 거부 호출을 시작했는지까지는 이번 trace가 구분하지 못한다.
2. **대기를 공유하는 위치** — `Runtime/Backend/DotNet/Wrappers/ZLinkBackendStreamSocketWrapper.cs:126–133`은 send를 공유 `_lane` 안에서 실행한다. `:351–360`에서 None은 binding `Submit()`으로 간다. 다른 session의 DONTWAIT closing과 heartbeat도 이 lane을 기다린다.
3. **binding은 계약대로 동작** — `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketOperations.Send.cs:26`, `Runtime/Messaging/CompletionOwner.cs:108`은 Core NONE admission을 호출한다. `bindings/doc/spec/dotnet/README.ko.md:340`이 `Submit()`=NONE, `Async()`=DONTWAIT completion을 규정한다. Binding/Core가 요청하지 않은 대기를 더했다는 증거가 아니다.
4. **실패 투영** — `Runtime/Streams/ZLinkStreamSessionRuntime.cs:477–492`는 닫힌 peer에 보낸 ServerDrain의 `NotConnected(errno 113)`도 `MarkTerminalFailed`로 바꾼다. `:432–446`과 `ZLinkStreamSessionTable.cs:98–111`에서 false로 모이고, `Runtime/Host/ZLinkFrameworkDrainExecutor.cs:256–275`가 `TeardownFailed`로 투영한다. 원래 예외는 해당 catch에서 보존하지 않는다.

`eventpipe-stacks-session-3/.../logs/server-session-0.stacks.jsonl`의 5114.864 ms 예외는 `SocketSendOperation.Submit → CompletionOwner.Send → ZLinkBackendStreamSocketWrapper.SubmitSend`다. 5115.380 ms 재throw는 `ZLinkStreamSessionTable.RejectNewSession`, 5115.419 ms는 `Could not submit the session-closing control packet.`까지 연결된다. **TrySubmit 자체의 1초 대기라고 오인하지 않는다.**

5.812초 사례인 `eventpipe-session-2`에서는 Backpressured가 trace 상대 시각 **5568.960, 6570.106, 7570.922, 8571.598, 9572.360 ms**에 반복됐다. 같은 window에 NotConnected(113), disconnect NotFound(605), closing-control 실패가 있다. 단순 CPU pause 설명보다 설정된 1000 ms blocking send가 직렬 누적되는 설명과 일치한다. 원래 phase 1의 각 SIGKILL PID가 모두 이 경로였다고 소급 확정한 것은 아니다.

### 3. 512 connections에서는 bounded teardown도 끝나지 않았다

`bounded-512-session-1`은 application echo 자체는 완료했지만 SIGTERM 뒤 75초에도 종료하지 않았다. 마지막 step은 `drain_stream_sessions → stream_session_drain_failed cancellation_requested=False → force_stop_begin reason=TeardownFailed budget=00:00:29.9978544`다. `Stopped` publication은 없었고 PID 49349를 SIGKILL했다. 기본 orderly 30초와 이 force budget을 합쳐도 75초를 설명하지 못한다.

`ZLinkDrainCoordinator.cs:493–496`의 force cancellation은 executor에 전달되지만, 위 blocking rejection send에는 token이 전달되지 않는다. `ZLinkStreamNodeRuntime.cs:122–129`의 component 정리도 session/control ingress 완료를 기다린다. 표준 send timeout이 요청마다 새로 적용되는 이 경로가 host 전체의 deadline을 따르지 않는 것이 **Framework의 bounded teardown 결함**이다. 진행 중인 모든 내부 await의 정확한 마지막 native frame을 확보한 것은 아니다.

첫 확대 재현의 SIGKILL로 `.nettrace` 끝이 잘려 전체 ETLX 변환은 실패했다. 보존된 prefix에서는 예외 2307건을 읽었고, Backpressured 222건과 closing-control InvalidOperation 73건이 있다(동일 예외의 재throw를 포함하므로 operation 수가 아니다). 마지막 Backpressured는 trace 상대 79085.757 ms다. 이 자료는 부분 trace로 표시하며 완결된 trace라고 취급하지 않는다. native gdb attach는 Yama/ptrace 제한으로 실패했고 전역 OS 설정은 바꾸지 않았다. 64-connection 세 실행의 완결된 CLR stack trace와 확대 재현의 결과를 구분해 보존했다.

### 소유 계약·교차언어·분류

- **소유 계층:** Framework의 session admission/closing 및 host drain. Core/binding의 NONE/DONTWAIT admission 결정은 그대로 재사용해야 한다.
- **Spec 조항:** host relocation `05-host-relocation-flow.ko.md:759` §14, 특히 `:768–787`의 seal·accepted work·deadline·transport teardown. D-097/D-098의 seal을 취소하거나 별도 monitor generation 상태를 만들지 않는다. Binding terminal 계약은 위 README:340이다.
- **교차언어 대조:** Node `runtime/streams/stream-session-runtime.ts:1361–1377`의 거부는 `closeForDrain`, `managed-stream.ts:153–175`의 control send는 awaitable `socket.submit`이다. Java `ZLinkStreamRuntime.java:1457–1474`도 `stream.sendAsync`를 사용한다. .NET의 공유 lane 안 blocking NONE 호출이 구조적 차이다. 다른 언어의 shutdown 정확성 전체를 검증한 것은 아니다.
- **변경 분류:** **B — 기존 Framework 결함 진단/수정 제안**, 구현 승인이나 runtime 수정은 수행하지 않았다. A의 timeout 원인은 분류 미확정이다.
- **수정 전/후 규칙 수:** 동일(제품 변경 0건). 하네스 timeout 확대와 종료 순서 변경을 대안으로 채택하지 않았다. 후속 구현은 기존 async submit/종료 owner와 host deadline을 재사용하는 방향으로 감독이 판단해야 한다.

## 공개 재현과 검증

재현은 기존 perf executable의 public `AddZLinkFramework`, typed echo, `RequestToChannel`, Stream Connector close와 ASP.NET Core lifetime을 사용했다. Runtime private API, raw frame 조작, 두 번째 socket poller를 사용하지 않았다. CLR EventPipe는 기존 예외를 관찰하는 외부 진단 도구다.

[재현 실행기](/dev/shm/zlink-perf-dotnet/diag-followup-20260906/repro-tools)와 각 셀의 `role-configs`, `endpoints.json`, `tmp/*-process.json`이 정확한 입력/명령/PID를 소유한다. `.NET trace` tool 10.0.731102는 `/tmp/zlink-perf-diag-tools`에만 설치했다. 기존 Release DLL을 재사용하는 실행기이며, 새 build가 필요하면 세 role 프로젝트를 지정된 build lock 안에서 먼저 빌드한다. 현재 Core/package hash 일치도 시작 전에 검사한다.

다음은 **새 output root**에서의 공개 사용 경로 재현 명령이다. output root를 재사용하지 않는다. 실행기가 samples lock을 소유한다.

```bash
source framework/languages/dotnet/perf/scripts/dotnet-env.sh
export ZLINK_PERF_DIAG_OUTPUT=/dev/shm/zlink-perf-dotnet/followup-replay-20260906-a
python3 /dev/shm/zlink-perf-dotnet/diag-followup-20260906/repro-tools/zlink-perf-followup-load.py quiet-repro 1
# 4096 / RouteMesh / 16 streams / inflight 1 / warmup 5 s / measured 30 s

export ZLINK_PERF_DIAG_OUTPUT=/dev/shm/zlink-perf-dotnet/followup-replay-20260906-b
python3 /dev/shm/zlink-perf-dotnet/diag-followup-20260906/repro-tools/zlink-perf-followup-eventpipe-stacks.py
# Session 64 / 4096 / 1+2 s, SIGTERM 및 기존 shutdown/CLR 예외 trace 관찰

export ZLINK_PERF_DIAG_OUTPUT=/dev/shm/zlink-perf-dotnet/followup-replay-20260906-b512
python3 /dev/shm/zlink-perf-dotnet/diag-followup-20260906/repro-tools/zlink-perf-followup-bounded-complete.py
# 별도 512-connection 확대 재현; timeout 값은 그대로다.
```

B 진단 실행기만 SIGTERM 후 **75초까지 관찰한 뒤 소유 PID를 정리**했다. 이는 Framework의 30초 deadline을 관찰하기 위한 외부 측정 범위다. 제품 `runner.py`의 5초 cleanup이나 application timeout을 고친 것이 아니다. 기존 phase1 보고서의 canonical shell 명령으로 실행하면 5초 경계에서 먼저 SIGKILL될 수 있어 최종 host 결과를 잃는다.

검증 결과:

- perf `ZLink.Framework.Perf.Tests`: **11 passed**, 실패 0. reset/drain, public-state counter lock 등의 기존 contract 포함.
- `perf/scripts/test_harness.py`: **9 passed**, 실패 0.
- 제품 코드 수정이 없으므로 회귀 test를 추가하지 않았다. CLR trace reader의 최초 dependency 누락은 도구 프로젝트에서 해결했고 최종 build 0 warnings/errors다. repo runtime build/test를 완화하지 않았다.
- 표준 perf 결과의 `valid`는 echo 수집 결과다. shutdown 실패를 정상 graceful 종료로 읽으면 안 된다. 확대 재현도 `result.json`만 보면 valid이고 `shutdown-observation.json`/`cleanup.json`에는 -9이므로 둘을 함께 본다.

## BLOCKERS와 후속 조치

1. **A 원인 미확정:** 새 9회에서 재현되지 않았다. 실패가 찍힌 message-flow/native 진행 증거가 없으므로 특정 HWM·reply route·admission 결함 또는 load-sensitivity로 확정할 수 없다. 원래 실패 16건은 해소됐다고 표시하지 않는다.
2. **D-118 library 미갱신:** N0는 수정 전 Core다. 감독이 Core rebuild/package 갱신 후 별도 동일 조건 검증을 해야 한다. 이 진단에서 Core나 package를 갱신하지 않았다.
3. **B Framework 구현 차단:** session rejection의 blocking send, terminal 오류 보존 및 전체 shutdown deadline 수렴은 `src/` 소유다. 이 job의 쓰기 금지 범위이므로 구현하지 않았다. B 분류로 감독에게 넘긴다.
4. **RouteMesh SIGTERM 별도 미재현:** 기존 PID 91811의 마지막 drain step은 보존되지 않았다. 이번 Session 결함을 그 PID의 원인으로 대체하지 않는다.
5. **원본의 관찰 한계:** 기존 5초 SIGKILL 기록만으로 당시 Framework의 30초 deadline 초과를 증명할 수 없다. 확대 재현 첫 trace는 SIGKILL로 끝이 잘렸다는 한계를 위에 적었다.

변경 파일: 이 보고서만. 제품 하네스 수정: 없음. 남은 실패: A 원인 미확정, B의 Framework session shutdown/오류 투영 결함, 원래 RouteMesh 종료 실패 미재현. Commit 없음.
