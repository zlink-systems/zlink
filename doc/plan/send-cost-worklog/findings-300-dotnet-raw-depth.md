# .NET raw 깊이 제한 — 조사 결과

## 1. raw request 한 건이 지나는 경로

1. `request-backpressure`는 `RunActiveAsync`에서
   `RunRequestBackpressureAsync`를 선택한다
   (`framework/bench/grpc/dotnet/Client/Program.cs:129-131`). 이 loop는
   `ExecuteRequestAsync` task를 계속 `pending`에 넣고, 256건마다 완료 task만
   제거한 뒤 `Task.Yield()`한다 (`:217-229`). 여기에는 request-window나
   `SEND_CONCURRENCY` 판정이 없다.
2. `ExecuteRequestAsync`는 payload와 source metric을 만든 뒤
   `IBenchTransport.RequestAsync`를 await한다 (`:307-317`). raw 구현은
   `RawBenchTransport.RequestAsync`이고, static envelope header와 protobuf body를
   만든 뒤 `RawBenchSocket.SubmitRequestAsync`를 await한다 (`:608-622`, `:658-670`).
3. request 시나리오의 raw transport는 socket 하나만 만든다. `send-saturation`일
   때만 `SEND_CONCURRENCY` 수의 command socket을 만들며, request 경로는
   `request` 하나다 (`:570-588`). 따라서 `SEND_CONCURRENCY=8`은 이 경로의 깊이를
   만들거나 제한하지 않는다.
4. `RawBenchSocket.SubmitRequestAsync`는 `SemaphoreSlim(1, 1)`을 얻은 뒤
   `Request().Message(header).Message(body).Async(...)`를 호출한다
   (`:674-680`, `:711-718`). 결과가 `Backpressured`이면 같은 permit을 잡은 채
   `submission.Admitted`를 await하고, 그 뒤에야 permit을 release하고 submission을
   반환한다 (`:719-726`). 그러므로 raw transport의 `:620` reply await와 caller의
   `ExecuteRequestAsync` 완료 관찰은 admission이 끝날 때까지 시작하지 못한다.
5. binding operation은 `DealerRequestOperation.Async` 또는
   `RouterPeerRequestOperation.Async`에서 `RequestCore`로 들어간다
   (`bindings/dotnet/src/Zlink/Runtime/Sockets/SocketOperations.Request.cs:36-43`,
   `:97-104`). `CompletionOwner.RequestAsync`는 `_submitSync` 안에서 completion
   entry를 등록하고 native request를 `DontWait`로 제출한다
   (`bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:119-145`).
   Core가 writable token을 돌려준 backpressure면 payload를 보관하고
   `RequestSubmission(Backpressured, entry.Admitted, entry.Task)`를 반환한다
   (`:148-167`). 즉 binding API는 admission task와 reply task를 별도로 이미
   제공한다.
6. 이 raw caller가 public completion owner를 설정하지 않았을 때 binding은 자체
   `POLLCOMPLETION` poller를 만들고 (`CompletionOwner.cs:555-580`), shared managed
   scheduler의 `Task.Run` pump를 시작한다 (`:584-609`). pump는 25 ms 이하로
   poll하고 (`:612-669`), drain은 `_submitSync` 아래에서 native completion을
   수거한다 (`:737-752`). `RequestCompletionEntry.Capture`는 writable completion을
   retry로 넘기고 (`:1218-1265`, `:1325-1366`), request completion의 reply를 task에
   설정한다 (`:1267-1320`).

## 2. 깊이를 제한하는 지점

- **어디** — `framework/bench/grpc/dotnet/Client/Program.cs:679`,
  `:714-726`.

- **무엇이 몇으로 묶는가** — request 행은 `:581-586`에서 socket을 정확히 하나
  만들고, 그 socket의 `submissionGate`는 `new(1, 1)`이다. 계산하면
  `request sockets 1 × gate permits/socket 1 = admission을 기다릴 수 있는 raw
  submitter 1`이다. 그 submitter가 `Backpressured`를 받으면 `:719-720`에서 permit을
  반환하기 전에 `Admitted`를 기다리므로, 그 구간의 **추가 native submit은 0건**이다.
  뒤따른 `ExecuteRequestAsync`는 `:714`에서 gate를 기다릴 뿐 binding에 도달하지
  않는다.

  `132`는 코드에 들어 있는 상수나 semaphore 용량이 아니라, 기준선이
  `SourceMetrics`에서 읽은 peak task 수다. `SourceMetrics.Begin`/`Complete`는 task
  수를 센다 (`Client/Program.cs:783-810`). 현재 보존된 source에는
  `132 = HWM bytes / request charge`를 계산할 당시의 applied HWM, queue byte
  accounting, `Backpressured` 시점 또는 run별 in-flight 원본이 없다. 따라서 132가
  정확히 첫 backpressure 순간의 native admitted request 수였는지도 확인하지 못했다.
  확인한 코드만으로 계산 가능한 값은 **1개의 admission gate와 backpressure 중
  0개의 후속 native submit**까지다. 132를 Auto-HWM 상수라고 쓰거나 그 값을
  역산하는 것은 근거가 없다.

- **정본은 어떻게 하는가** — 세 하네스 모두 submission과 reply completion을
  분리한다.

  - C는 한 socket sweep에서 `submit_request` 결과를 받고, backpressure면 socket에
    retained request/token만 남긴다
    (`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:290-334`). turn 끝에는
    `submitted ? 0 : poll_timeout_until(deadline, 50)`로 completion poller를 한 번
    진행한다 (`:576-603`; timeout helper `:123-134`).
  - .NET binding perf는 `RequestSubmission`을 즉시 받아 `submission.Reply` 관찰을
    시작하고 (`bindings/dotnet/perf/multi/Zlink.BindingBench.Multi/src/PerfMultiSocketReqRep.cs:371-374`),
    `Backpressured`일 때만 그 socket을 `AdmissionPending`으로 표시해
    `submission.Admitted`를 별도 task로 관찰한다 (`:375-379`, `:342-355`). 매 turn
    public completion poller를 `submittedAny ? 0 : RemainingPollTimeoutMs(...)`로
    진행한다 (`:358-392`), 후자는 최대 50 ms다 (`:416-423`).
  - Node도 `submission.reply`를 즉시 pending set에 넣고 (`bindings/node/perf/multi/perf_multi_socket_reqrep.ts:144-147`),
    backpressure는 `available[index] = false`와 별도 `submission.admitted` task로
    보관한다 (`:148-155`). C turn과 같다는 주석 바로 아래에서 모든 socket이 blocked인
    경우만 `pollTimeoutUntil(activeStopNs, 50)`, 아니면 0으로 completion poller를
    진행한다 (`:159-167`; helper `:47-52`).

  대상 raw는 이 셋과 달리 `RequestSubmission`을 caller에 반환하기 전에 admission을
  await한다. 그 때문에 `RunRequestBackpressureAsync`의 unbounded task 제출은 binding
  submit/reply progress를 직접 turn으로 소유하지 못하고, raw socket gate가 admission
  대기를 단일 전역 제출 정지로 만든다.

- **고치는 방법** — raw request 벤치를 정본과 같은 turn 소유자로 바꾼다. 즉,
  `RawBenchSocket`은 `RequestSubmission`을 즉시 반환하고, caller가 (1) `Reply`를
  pending completion으로 등록하고, (2) `Backpressured`면 그 socket의
  `Admitted`만 admission-pending으로 보관하며, (3) 한 submit turn 뒤 단 하나의
  `POLLCOMPLETION` poller를 `submitted ? 0 : poll_timeout_until(deadline, 50)`로
  진행해야 한다. admission 완료 뒤 다음 제출을 재개한다. 이 변경은 request-window나
  새 application 상한을 추가하지 않는다. `SemaphoreSlim`으로 admission await를
  감싸는 경로는 제거 대상이다.

- **위험** — public poller가 binding의 runtime pump와 동시에 같은 completion queue를
  drain하면 completion 소유권이 깨진다. binding은 public owner가 없을 때만 runtime
  poller를 만들도록 되어 있으므로 (`CompletionOwner.cs:561-563`, `:592-595`), 구현 전
  public poller 등록이 그 owner 전환을 정확히 수행하는지 확인해야 한다. 또한
  `Backpressured` request의 retained payload와 cancellation, drain 종료 시 모든
  `Reply`/`Admitted` task를 함께 끝내는 binding 계약을 보존해야 한다.

## 3. 4 KiB에서 G5 31.4%로 실패하는 것도 같은 원인인가

확인하지 못했다. 5-run 요약은 4 KiB request-backpressure의 G5 31.4%만 기록하고
(`doc/plan/backpressure-single-counter.ko.md:492-501`), 각 run의 raw peak in-flight,
첫 `Backpressured` 시점, applied HWM 및 queue bytes를 함께 보존하지 않는다. 따라서
같은 raw request 코드가 모든 payload 크기에서 위 gate를 사용한다는 사실은 확인했지만,
4 KiB의 run 간 분산을 이 gate가 만들었다는 인과는 이 자료만으로 확정할 수 없다.

## 4. 확인 못 한 것

- 기준선 5 run 각각의 source result JSON, raw context Auto-HWM snapshot 및 socket
  monitor snapshot이 저장소에 없다. 그래서 132의 정확한 native admitted request 수와
  byte-HWM의 대응을 계산하지 못했다.
- 이 조사에서는 빌드·벤치·계측을 하지 않았다. public completion poller를 raw socket에
  등록할 때 binding runtime pump가 어떤 public-owner 전환 API를 거치는지는 구현 전에
  source와 focused test로 확인해야 한다.
- 4 KiB G5 분산과 gate의 인과는 run별 peak/submit-result/admission 기록을 같은 측정
  창에서 대조하기 전까지 확정하지 못한다.
