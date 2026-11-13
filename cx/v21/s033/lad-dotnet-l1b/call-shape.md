# .NET ladder L0/L1 호출 형태 대조

L0 서버는 기준 `a542ee2384`의 `ZLinkDotnetLadderLevel=L0` 산출물, L1 서버는 `b6f58168a6`의 `ZLinkDotnetLadderLevel=L1` 산출물이다. Client Release DLL은 세 경우에 공통이며, 측정은 `zlink-dotnet`의 같은 raw client 구현(`BenchMeasurement.cs:273`, `Program.cs:162-166`)을 쓴다. `ZLINK_DOTNET_RAW_TARGET`은 target endpoint와 routing metadata만 설정한다. 서버 DLL은 L0/L1 별도 보관했다.

| 단계 | Binding 호출 (`file:line`) | L0 호출 (`file:line`) | L1 호출 (`file:line`) | 같은가? 차이가 있으면 복원 Framework 기능 |
|---|---|---|---|---|
| Source send (request/send) | `Client/Program.cs:470-490,557-564,678-685`; send `:513-527,687-701`; `frameworkTarget=false` | 같은 raw methods; `frameworkTarget=true` (`:162-170,385-391,414-425,479-490`) | 같은 Client DLL/methods/configuration as L0 | 호출 수·part 수·body encode는 같음. envelope header의 `formatMarker`와 destination routing id만 Framework target에 맞춰 다름 (`Shared/RawEnvelopeHeaders.cs:5-22`, `Program.cs:385-425`); Framework envelope/routing metadata |
| Target receive | `ZLinkRawServer/Program.cs:64-76,125-137`; blocking `router.Recv(received)`, 100ms timeout `:37-46,176` | `ZLinkServer/Program.cs:108-118,126-147`; blocking `router.Recv(received, RecvFlags.None)`, 100ms timeout `:49` | `ZLinkServer/Program.cs:159-172,182-208`; same blocking one-record `Recv(None)`, 100ms timeout; permit acquire `:165` | wait primitive/loop/drain 같음. L0의 fixed handler 직접 호출(`:116-118`)과 L1의 acquire/handler 시작 전 release(`:165,171,178`)가 복원 기능 |
| Target reply | `ZLinkRawServer/Program.cs:77-85,103-108` | `ZLinkServer/Program.cs:116-123,259-267` | `ZLinkServer/Program.cs:169-174,259-267` | 같음: payload parse → 새 protobuf body encode → 새 response header + body 2-part submit. 요청 body 재사용 없음 |
| Source completion / reply receive | `Client/Program.cs:485-490,572-595`; `RequestSubmission.Reply`, decode, parts/header/body dispose | 같은 raw client methods | 같은 raw client methods | completion poll, Reply 관찰, protobuf decode 같음 |

## Per-message waits / allocations / atomics / copies

| 경로 | Binding | L0 | L1 |
|---|---|---|---|
| Request receive/reply | blocking Recv 1회; parse 1회; 새 reply payload/body/header 및 2-part submit; 같은 metrics counter; request body copy 없음 | receive/parse/encode/message/submit/counter 같음; direct handler | L0 + lease object, acquire/release state-lane turn, lease `Interlocked.Exchange` 1회 (`ZLinkApplicationJobQueue.cs:208-223,520-534,1210-1211`); message copy 없음 |
| Send receive | blocking Recv 1회; parse 1회 및 같은 metrics counter; reply 없음 | 같은 receive/parse/counter; direct handler | L0 + lease object, acquire/release state-lane turn, lease `Interlocked.Exchange` 1회 (`ZLinkApplicationJobQueue.cs:208-223,520-534,1210-1211`) |
| Source | 같은 raw methods, header/body allocation, protobuf body encode, 2-part submit, completion poll/decode. Framework target은 envelope header가 더 길고 route id가 다름 | 동일한 Framework-target raw client | L0와 동일 |

판정: `BenchServerMetrics.cs:50-70`의 동일한 metrics lock 외에 binding/L0의 성공 경로 per-message atomic은 없다. L1의 lease 반환은 `Interlocked.Exchange` 1회다. L1과 L0 서버의 차이는 permit 획득을 receive 앞에 두고 handler 첫 명령 전에 반환하는 것뿐이다. Binding과 ladder 사이에는 Framework target route/envelope metadata가 다르며 이는 Framework target protocol 경로에 속한다. 나머지 receive/reply 호출 형태는 일치해 측정 가능하다.
