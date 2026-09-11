# Framework gRPC 비교 bench 5언어 캠페인 — 결정 기록

계획: [`../framework-bench-with-grpc-5lang-plan.ko.md`](../framework-bench-with-grpc-5lang-plan.ko.md)

각 항목은 결정, 근거, 적용 위치를 남긴다. 번복하면 새 ID로 기록하고 옛 항목에 번복 표시를 단다.

## FB-001 — raw binding 행을 ROUTER↔ROUTER로 통일한다

- **결정** (2026-09-06, 사용자): ZLink raw binding 행의 소켓을 DEALER→ROUTER에서 ROUTER↔ROUTER로
  바꾼다. `bindings/c/bench/with_grpc`의 `zlink-c` 기준값도 같은 구성으로 맞춘다.
- **근거**: framework 행은 RouteMesh ROUTER↔ROUTER인데 raw 행이 DEALER→ROUTER면
  `zlink-framework-<lang> / zlink-<lang>` 비율에 framework 계층 비용과 소켓 패턴 비용이 함께
  들어간다. 규격 §7의 0.80 판정이 framework 계층만 재려면 두 행이 같은 패턴이어야 한다.
- **확인한 코드**: framework 서버 `framework/languages/dotnet/bench/with-grpc/ZLinkServer/Program.cs:20-24`
  가 `AddRouteMesh` + `AddRequestHandler` + `AddSendHandler`. raw client
  `framework/languages/dotnet/bench/with-grpc/Client/Program.cs:493,512,530`이 DEALER.
  raw server `ZLinkRawServer/Program.cs:25-28`은 이미 ROUTER.
  C 기준 bench는 `bindings/c/bench/with_grpc/zlink/bench_zlink_client.cpp:530-531`이 DEALER,
  `bench_zlink_server.cpp:273-274`가 ROUTER.
- **적용**: Phase 0(job `fwb-02`).

## FB-002 — send 비교는 gRPC unary `Command` → `Empty`를 유지한다

- **결정** (2026-09-06, 사용자): `send-saturation`의 gRPC 대응물을 현행 unary로 둔다.
  client-streaming 셀을 추가하지 않으며 proto에 RPC를 더하지 않는다.
- **경위**: 감독관이 "gRPC는 왕복, ZLink는 단방향이라 비대칭"이라는 우려를 제기하고
  client-streaming 추가를 제안했다. 사용자가 이 bench의 목적은 **서비스 측면 비교**이며,
  같은 업무를 각 스택으로 구현했을 때의 비용을 재는 것이라고 확정했다.
- **근거**: 실제 서비스에서 응답이 필요 없는 호출은 unary + `Empty`로 구현한다.
  client-streaming은 업로드·대량 적재용이며 단발 명령에 쓰지 않는다. gRPC에 단방향 호출
  원시 기능이 없어 왕복을 치르는 것은 gRPC의 특성이고, 서비스 측면 비교에서는 그 비용이
  결과에 그대로 드러나는 것이 맞다.
- **서술 제약**: 이 셀을 전송 속도 차이로 쓰지 않는다. "응답이 필요 없는 명령을 처리할 때
  gRPC는 unary 왕복을 치르고 ZLink는 단방향 send로 끝난다. 이 조건에서 차이는 N배였다"가
  정확한 문장이다.
- **적용**: Phase 1(job `fwb-01`) 규격 문서, Phase 6 보고서 서술.

## FB-003 — 판정 패턴과 기준식은 기존 규격을 따른다

- **결정** (2026-09-06): 판정 기준 패턴은 `request-window`, 기준식은 규격 §7의 두 식
  (`zlink-<lang> / zlink-c >= 0.80`, `zlink-framework-<lang> / zlink-<lang> >= 0.80`)을 그대로 쓴다.
- **근거**: `request-window`에서 gRPC unary `Echo`와 ZLink request는 서버 처리 확인이라는
  같은 보장을 주므로 정면 비교가 성립한다. 기존 규격을 이 캠페인이 바꾸지 않는다.

## FB-004 — 두 ZLink 행의 endpoint 개수 차이는 그대로 둔다

- **결정** (2026-09-06, 감독관): raw binding 행은 request echo endpoint와 command endpoint를
  분리하고 framework 행은 RouteMesh 연결 하나가 request 처리기와 send 처리기를 함께 가진다.
  이 차이를 맞추지 않는다.
- **경위**: job `fwb-01`이 FB-001로도 해소되지 않는 비대칭으로 보고했다.
- **근거**: 패턴을 한 번에 하나씩 측정하므로 `send-saturation` 셀에서는 두 행 모두 send만
  전달하고 request 셀에서는 request만 전달한다. 따라서 어느 셀도 오염되지 않는다.
  raw 쪽 분리는 두 패턴이 같은 socket을 공유할 때를 대비한 구성이다.
- **제약**: 한 셀에서 두 패턴을 동시에 실행하는 시나리오가 생기면 이 전제가 깨진다.
  그때 구성을 다시 결정한다. 규격에 이 전제를 명시한다.
- **적용**: job `fwb-01` 후속 지시.

## FB-005 — 0.80 판정은 두 payload 크기 모두에서 만족해야 한다

- **결정** (2026-09-06, 감독관): 규격 §7.2의 두 판정식은 payload 크기별로 적용한다.
  `1024`와 `4096` **둘 다** 만족해야 그 언어가 통과다. 한 크기만 만족하면 통과가 아니다.
- **경위**: job `fwb-01`이 기존 규격에 payload 한정자가 없다고 보고했다.
- **근거**: 1024에서 유지되다가 4096에서 무너지는 구성은 실제 문제다. 보고서가 어차피 두
  크기를 모두 싣기 때문에 크기별 판정은 추가 비용이 없고 정보를 감추지 않는다.
- **적용**: job `fwb-01` 후속 지시, Phase 6 보고서 주 표.

## FB-006 — .NET with-grpc bench는 커밋된 상태로 빌드되지 않았다 (기존 결함)

- **관찰** (job `fwb-02`, 감독관 검증 완료): `ZLinkRawServer/Program.cs`가
  `received.RequestSeq`를 사용하는데 `bindings/dotnet`의 `Received`에는 그 멤버가 없다.
  현재 계약은 `Received.ReplyToken`이다(`bindings/dotnet/src/Zlink/Contracts/Messaging/Received.cs:58`).
  `RequestSeq`는 커밋 `bf2da527dc`에서 사라졌다.
- **영향**: `run_local.sh:19`가 이 프로젝트를 빌드하고 스크립트는 `set -euo pipefail`이므로
  **runner 전체가 빌드 단계에서 실패한다.** 즉 .NET with-grpc bench는 커밋된 상태로 한 번도
  실행되지 않았다. `log/` 디렉터리가 비어 있던 것과 일치한다.
- **처리**: `received.ReplyToken is not null`로 수정했다. binding 계약 변경에 bench가 따라오지
  않은 경우이며 계약 쪽 문제가 아니다.
- **후속**: bench가 어떤 정기 gate에도 걸려 있지 않아 이 파손이 드러나지 않았다. 5언어 확장
  뒤 bench를 어떤 주기로 돌릴지는 0.18.0 후보로 올린다.

## FB-007 — C 기준 bench의 server CPU·memory 값은 구조적으로 0이었다 (기존 결함)

- **관찰** (job `fwb-02`, 감독관 검증 완료): `bindings/c/bench/with_grpc/run_local.sh`가
  `setsid "${server}" & ; pid=$!`로 서버를 띄웠다. `setsid`는 호출자가 프로세스 그룹 리더가
  아니면 fork하므로 `$!`는 곧 사라지는 wrapper의 PID다.
- **영향**: client가 `SERVER_PID`로 서버 CPU·RSS를 표본화하는데 그 PID가 서버가 아니었다.
  따라서 C bench가 낸 server CPU·memory 열은 전부 무의미한 값이었다. cleanup이 그 그룹을
  죽여도 서버에 닿지 않는 문제도 함께 있었다.
- **처리**: 각 서버가 자기 PID를 pidfile에 기록하고 runner가 그 값을 읽도록 바꿨다.
- **주의**: 과거 C bench 결과의 server CPU·memory 수치를 이 캠페인의 비교에 인용하지 않는다.

## FB-008 — 셀 사이 backlog 전이를 settle이 막아야 한다

- **관찰** (job `fwb-02`): `zlink-framework-dotnet-request-serial` @4096 셀이 run 1과 run 3에서
  `zlink error code 101` TimedOut으로 실패했다. 원인은 시작 경합이 아니라 **앞 셀의 잔여
  backlog**다. 바로 앞 셀 `zlink-framework-dotnet-send-saturation`은 5초 × 8-way flood이고
  framework send의 p95는 약 718 ms(같은 조건 raw는 1.7 ms)다. client의 settle이
  `--command-settle-ms` 기본 200 ms로 고정돼 있고 서버가 비워지지 않아도 그대로 반환하므로,
  다음 패턴의 첫 request가 남은 backlog 뒤에 줄을 서서 framework request timeout을 넘긴다.
- **결정** (2026-09-06, 감독관): settle은 고정 시간 대기가 아니라 **서버가 비워졌음을 확인하는
  유한 대기**여야 한다. 상한 안에 비워지지 않으면 그 사실을 기록하고 **다음 셀을 오염된 것으로
  표시**한다. 조용히 측정해서 표에 싣지 않는다.
- **근거**: 규격 어디에도 settle 값이 없다. `--command-settle-ms`는 client 구현 세부이지
  고정 계약값이 아니므로 이 수정은 측정 조건 변경 금지에 걸리지 않는다. 반대로 지금 상태를
  두면 셀 하나가 다음 셀을 죽이는 harness를 4개 언어에 그대로 복제하게 된다.
- **FB-004와의 관계**: FB-004는 두 ZLink 행의 endpoint 개수 차이가 셀을 오염하지 않는다고
  판단했고 그 근거(한 번에 한 패턴만 측정)는 여전히 유효하다. 다만 그때 고려하지 않은
  오염 경로가 하나 더 있었다. **같은 프로세스에서 이어 도는 셀 사이의 서버 backlog 전이**다.
  FB-008이 그 경로를 막는다.
- **적용 범위 정정** (2026-09-06, job `fwb-02` 제안을 감독관이 수용): 오염 표시는 "순서상
  다음 셀"이 아니라 **같은 server를 쓰는 다음 셀**에 한정한다. 구현마다 server process가
  따로 있고 framework backlog는 framework 셀만 건드린다. 실제로 죽은 셀 사이에 있던
  grpc·raw 셀은 영향을 받지 않았다. 순서 기준으로 표시하면 멀쩡한 grpc 셀을 빼면서 정작
  오염된 framework 셀은 싣게 된다.
- **적용**: job `fwb-02`가 harness에 구현하고 규격 §3에 계약으로 추가한다. Phase 2~5의 네
  언어는 이 계약을 구현한 상태로 만든다. drain 상한(`--drain-bound-ms`, 기본 30 s)은
  정상 실행에서 걸리지 않을 만큼 커야 하며 관측한 drain 시간을 셀마다 기록한다.

### FB-008 관측: drain 시간 비대칭 (smoke, send 2초 기준)

| 셀 | @1024 | @4096 |
|---|---|---|
| `grpc-dotnet-send-saturation` | 224 ms | 213 ms |
| `zlink-dotnet-send-saturation` (raw) | 330 ms | 253 ms |
| `zlink-framework-dotnet-send-saturation` | **5700 ms** | **4820 ms** |

framework server는 2초짜리 send 셀 뒤 약 5초를 비우는 데 쓴다. 기존 200 ms 고정 settle은
약 5.5초 일찍 반환하고 있었다. 이 값은 harness 주석이 아니라 결과이므로 보고서에 싣는다.
drain 대기를 넣은 뒤 18셀 전부 완료, 실패 0, 오염 0.

## FB-009 — framework send 경로의 saturation 지연은 결과로 기록한다

- **관찰** (job `fwb-02`): `send-saturation` 조건에서 framework send의 p95가 약 718 ms,
  같은 조건 raw binding이 약 1.7 ms였다.
- **처리**: 이 캠페인은 원인을 고치지 않는다. 값을 그대로 보고서에 싣고 후속 후보로 올린다.
  saturation 셀의 지연은 sender가 receiver를 앞지를 때 자연히 커지므로 이 값 하나로 결함을
  단정하지 않는다. 판정은 KMSG/s로 하고 지연은 함께 기록한다.
- **후속**: 0.18.0 후보 — framework send 경로의 backpressure와 drain 특성.

## FB-010 — .NET raw request는 window 100 중 8.4만 유지한다 (harness 결함)

- **관찰** (job `fwb-02`): Little's law로 확인한 실제 in-flight 수.

  | 행 | 처리량/s | 평균 ms | in-flight |
  |---|---|---|---|
  | `grpc-c` | 64,567 | 1.529 | 98.7 |
  | `zlink-c` | 425,907 | 0.213 | 90.7 |
  | `grpc-dotnet` | 196,191 | 0.468 | 91.8 |
  | `zlink-framework-dotnet` | 3,017 | 32.818 | 99.0 |
  | **`zlink-dotnet` (raw)** | 37,113 | 0.225 | **8.4** |

- **원인**: `RunRawRequestBytesAsync`에서 동기적으로 완료된 request가 `pending`에 들어가지
  않아 `pending.Count`가 실제 깊이를 낮게 센다. 같은 harness가 같은 run에서 gRPC 91.8,
  framework 99.0을 유지하므로 window 로직 자체는 동작한다. raw ZLink 경로에 한정된 결함이다.
- **결정** (2026-09-07, 감독관): `zlink-dotnet / zlink-c` 두 셀을 **`unsupported`로 표시하고
  값을 인용하지 않는다.** 0.087·0.166은 window 8 실험을 window 91 실험으로 나눈 값이다.
- **적용**: Phase 2가 harness를 복제하기 **전에** 고친다. 고치지 않으면 네 언어가 같은
  결함을 물려받고 formula 1이 다섯 언어 전부에서 무의미해진다.

## FB-011 — 0.80 판정 전에 G5를 행 단위로 강제한다

- **관찰**: 3회 ROUTER run에서 gRPC 12행 전부와 `zlink-dotnet` raw request 4행은 ±7.7% 이내로
  통과하는데, ZLink 10개 행이 실패한다. 최악은 `zlink-c` request-window @4096의 **75.7%**
  스프레드(214.7 / 396.4 / 225.6 KOPS)다. 같은 run에서 다른 행들이 안정적으로 수렴하므로
  머신 잡음이 아니다.
- **결정**: 어떤 0.80 판정도 **분자와 분모가 모두 G5를 통과한 행**으로만 낸다. 통과하지
  못한 행으로 만든 비율은 중앙값으로 계산했더라도 싣지 않는다.
- **현재 상태**: 네 판정값 중 **양쪽이 G5를 통과하는 것은 하나도 없다.**

## FB-012 — saturation flood 뒤 framework route가 영구히 끊긴다 (framework 결함)

- **관찰** (job `fwb-02`): @1024 framework send-saturation flood 뒤 RouteMesh peer 연결이
  끊기고 재연결되지 않아 이후 framework 셀 세 개가 전부 실패한다.
  `Channel request to 'bench' failed because the target route is not connected.`
  `dotnet-router-1`과 `dotnet-dealer-1`에서 재현됐다.
- **성격**: harness 결함이 아니라 **framework 결함**이다. 이 캠페인은 고치지 않는다.
- **영향**: framework @4096 셀이 3회 중 2회만 측정됐다. 이 상태로는 framework의 4096
  거동을 신뢰할 수 없다.
- **후속**: 0.18.0 후보. saturation 뒤 RouteMesh peer 연결 유지와 재연결.

## FB-013 — send-saturation 처리량은 active window 경계에서 표본화한다

- **관찰** (job `fwb-02`): 현재 구현은 **drain이 끝난 뒤** server snapshot을 읽는다
  (`Client/Program.cs:510`, `:524`). 그래서 표의 값은 server의 소비율이 아니라
  "결국 server가 받은 것으로 걸러진 client 제출률"이다. framework @1024 run 2는 표에
  125.9 KOPS인데 실제 소비율은 629,403 / 21.235 s ≈ **29.6 KOPS**로, 표가 약 4.2배 부풀린다.
  gRPC·raw는 drain이 0.2~0.5초라 차이가 무시할 수준이다.
- **결정** (2026-09-07, 감독관): **규격 §5가 이미 "server가 active phase에서 받은 messages
  수"라고 정하고 있다.** 현재 구현이 그 계약을 어기고 있는 것이므로, 규격을 바꾸는 것이
  아니라 구현을 규격에 맞춘다. snapshot을 **active window가 닫히는 시점**에 읽고, drain은
  settle 용도로만 쓴다. 관측한 drain 시간은 셀마다 따로 기록한다.
- **효과**: framework send 값이 크게 내려간다. 그것이 그 스택이 실제로 지탱하는 소비율이다.

## FB-014 — C bench의 send-saturation은 client 제출 수를 센다 (G3 실패)

- **관찰** (job `fwb-02`): `run_send_loop`가 `r.completed = seq`로 client 자신의 제출 수를
  기록한다. 규격 §5는 server 수신 수를 요구한다. C harness에는 server stats endpoint가 없다.
- **결정**: `zlink-c`·`grpc-c`의 send-saturation 4셀을 **판정에서 `unsupported`로 둔다.**
- **성격**: 기존 결함이며 이 캠페인이 만든 것이 아니다.

## FB-015 — FB-010을 대체한다. @1024 판정을 실패로 게재한다

- **경위**: FB-010은 `zlink-dotnet / zlink-c` 두 셀을 `unsupported`로 두었고, 근거는
  "window 8 실험을 window 91 실험으로 나눈 값"이었다. job `fwb-02b`가 그 전제를 반증했다.
- **결정** (2026-09-07, 감독관): **@1024는 `0.084`를 실패 결과로 게재한다.** 깊이 8은
  harness 결함이 아니라 .NET raw binding의 실제 성질이고, formula 1은 바로 그것을 재려고
  존재하는 식이다. 분자·분모가 모두 G5를 통과하므로 FB-011도 만족한다. 원인이 규명된
  실패값을 감추는 것이 오히려 정보를 버리는 일이다.
- **@4096은 `unsupported`로 유지한다.** 분모 `zlink-c` request-window @4096이 G5 25.7%로
  실패한다. FB-005에 따라 두 크기 모두 만족해야 하므로 **`dotnet`은 통과가 아니고
  완전한 판정도 불가능하다.**

## FB-016 — .NET raw request 경로는 단일 스레드 제출 비용에 묶인다 (측정 결과)

- **판정** (job `fwb-02b`, 증거 4종): 동시성 상한이 아니다. `peak_in_flight`를 직접 계측한
  결과 6개 셀 중 4개가 **100/100**에 도달했고 abandoned=0이다. submit 경로
  (`SocketKernel.MultipartSubmit.cs`)에 lock·semaphore·channel·Monitor가 없다. gRPC window
  루프는 수정 전 raw 루프와 구조가 같은데 같은 run에서 깊이 93.6을 낸다.
- **메커니즘**: 요청당 client CPU 비용이 한계다.

  | 행 | 처리량/s | client cores | µs CPU/request |
  |---|---|---|---|
  | `zlink-c` | 430,617 | 1.74 | **4.0** |
  | `grpc-c` | 65,026 | 1.76 | 27.1 |
  | `zlink-dotnet` | 36,034 | **1.16** | **32.2** |
  | `grpc-dotnet` | 198,787 | **7.38** | 37.1 |

  예측 깊이 = 왕복 지연 ÷ 제출 비용 = 221 µs ÷ 32.2 µs = 6.9 (관측 8.0).
  `grpc-dotnet`은 요청당 비용이 더 큰데도 7.38 코어로 퍼져 5.5배 처리량을 낸다.
  **ZLink raw 경로는 단일 스레드에 묶여 있고 gRPC 경로는 그렇지 않다.**
- **후보 위치**: Core의 socket-local attempt gate(07-router §12). **계측하지 않았으므로
  단정이 아니라 후보로만 기록한다.**
- **처리**: 이 캠페인은 고치지 않는다. 0.18.0 후보.

## FB-017 — `peak_in_flight` 계측을 다섯 언어 harness에 넣는다

- **근거**: 이 한 줄이 "harness가 window를 못 채운다"와 "스택이 그 깊이까지만 낸다"를
  구분한다. 없었다면 FB-010의 잘못된 전제를 계속 안고 갔을 것이다.
- **적용**: Phase 2~5의 네 언어 harness가 셀마다 도달 깊이와 abandoned 수를 출력한다.

## FB-018 — `grpc-c` request-serial의 G5 급락 (기록만)

- **관찰**: 이전 pass에서 7.7%/2.0%로 통과하던 행이 이번 pass에서 173.8%/165.0%로 실패했다.
  run마다 server를 새로 띄우는데 14.6 → 5.3 → 4.0으로 첫 run만 높은 모양이다.
- **처리**: 판정 경로에 없으므로 조사하지 않고 기록만 한다.

## FB-012 갱신 — 간헐이며 결정적이지 않다

`fwb-02b`의 ROUTER 3회에서는 재현되지 않았고 DEALER run에서만 나왔다. "flood 뒤 항상
끊긴다"는 이전 판독은 약해진다. framework 결함이라는 성격과 0.18.0 후보 처리는 유지한다.

## FB-019 — 규격 §5.1의 포화 판정은 현재 규칙으로 발동하지 않는다

- **관찰** (job `fwb-03`): `Client CPU`는 두 runner 모두 **논리 코어 20개 전체 대비 비율**로
  기록한다. `gated2` 전체에서 가장 큰 값이 36.9%(= 7.38 코어)다. 단일 thread인 Node client가
  코어 하나를 완전히 채워도 약 **5%** 로 읽힌다. 즉 §5.1이 겨냥한 바로 그 경우에서
  95% 임계값은 영원히 걸리지 않는다. 임계값과 지표의 척도가 다르다.
- **결정** (2026-09-07, 감독관): 규격 §5.1을 고친다.
  1. 표는 client CPU를 **사용 코어 수**로도 싣는다(백분율만으로는 판독이 불가능하다).
  2. 포화 판정은 **각 언어 harness가 선언한 client 병렬도 상한** 대비로 한다. 사용 코어 수가
     그 상한의 0.95배 이상이면 포화로 표시하고 처리량 순위에서 제외한다.
  3. 병렬도 상한은 언어마다 다르므로 결과에 그 값을 기록한다. Node는 단일 thread이므로 1이다.
- **근거**: 판정의 목적은 "transport가 아니라 client 런타임이 상한이었다"를 잡는 것이다.
  전체 코어 대비 비율로는 단일 thread 포화를 볼 수 없다.
- **적용**: Phase 2 이전. 고치지 않으면 **Node가 client에 묶인 값을 포화 표시 없이 게재한다.**

## FB-020 — 규격 §7.4의 단위 규칙을 집계기 정규화로 대체한다

- **결정**: §7.4 첫 문단(수동 1000 나누기)을 삭제하고, 처리량 단위는 §4가 초당 완료 수로
  고정하며 배율이 다른 runner의 값은 공용 집계기가 `bandwidth`(MB/s 고정)에서 역산해
  정규화한다는 문장으로 바꾼다. **비교와 판정은 언제나 집계기 출력으로 한다.**
- **둘째 문단은 유지한다.** "이 조건에서 더 빠르다/느리다로만 해석한다"는 단위가 아니라
  해석 제약이며 살아 있어야 한다.
- **조건**: 삭제가 안전한 이유는 비교가 집계기 출력으로 옮겨가기 때문이다. runner의
  `report.txt` 두 개를 손으로 비교하는 사람은 여전히 불일치를 만난다. 그 사실을 문장에 남긴다.
- **부수 결정**: 언어별 client의 자체 표는 단일 실행 편의로 남기되 **정본이 아니다.**
  판정 근거는 집계기 출력뿐이다.

## FB-021 — 진단값은 산문이 아니라 구조화된 데이터로 전달한다

- **관찰** (job `fwb-03`): `peak_in_flight`, abandoned, drain 시간, 경계 표본, 오염 표시가
  `.NET` stdout과 `failures.txt`에만 있고 `results.json`에는 없다. 집계기가 사람이 읽는
  텍스트를 정규식으로 긁어 payload를 절 표지로 구분한다. 가장 약한 고리다.
- **결정**: `fwb-03`이 정의한 `with-grpc-cell-v1`을 Phase 2~5의 표준 입력 통로로 삼고,
  `.NET` harness도 같은 필드를 `results.json`에 낸다. 측정을 바꾸지 않는 변경이다.

## FB-022 — 인수 fixture를 저장소에 둔다

- **결정**: `framework/bench/tools/tests/fixtures/gated2/`(113 KB)를 유지한다. 인수 조건이
  재현 가능해야 나중 언어가 Phase 0 기준을 우회할 수 없다. 측정 원본이 아니라 옵션 헤더와
  `RESULT`·진단 라인만 남긴 축약본이다.

## 정정 — 요약의 depth 98.6은 98.7이다

`bench-dotnet-summary.ko.md` §3.4의 `zlink-framework-dotnet` @4096 depth는 반올림된 표값
(2.458 KOPS)으로 계산해 98.649가 나왔다. 반올림 전 중앙값(2458.2/s)으로는 98.657 → **98.7**이다.
집계기가 맞고 요약이 틀렸다. 결론은 바뀌지 않는다.

## FB-023 — 포화 계측기는 언어마다 다르다. Node는 event loop 사용률을 쓴다

- **관찰** (job `fwb-04`): FB-019에서 감독관이 "Node의 병렬성 상한은 1"이라고 정했는데,
  `client_cores`를 프로세스 CPU ÷ 경과 시간으로 재면 binding의 native I/O thread가 함께
  잡혀 ZLink 행에서 **1.31~1.41 코어**가 나온다. 상한 1 기준으로는 거의 모든 셀이 포화로
  표시되어 표시가 정보를 잃는다.
- **감독관 정정** (2026-09-07): **FB-019의 상한 값이 아니라 계측기 선택이 틀렸다.**
  포화 판정의 목적은 "transport가 아니라 client 런타임이 상한이었다"를 잡는 것이고, Node에서
  그 상한을 정하는 것은 **user 코드가 도는 JS thread**다. binding의 native I/O thread는
  user 코드를 실행하지 않으므로 프로세스 CPU는 Node에 맞는 계측기가 아니다.
- **결정**: 각 언어 harness가 **무엇을 재는지와 그 상한을 함께 선언한다.**
  - Node: `perf_hooks`의 `performance.eventLoopUtilization()`, 상한 `1.0`. 이 값이 0.95
    이상이면 포화다. 이 환경에서 사용 가능함을 확인했다.
  - `.NET`·Java·Kotlin·C++: 프로세스 사용 코어 수, 상한은 harness가 선언한 병렬도.
- **적용**: 규격 §5.1을 계측기 선언까지 포함하도록 고친다. 집계기는 선언된 계측기와 상한을
  그대로 읽어 0.95 규칙을 적용한다.

## FB-024 — 규격 §3의 raw wire 서술이 두 구현과 어긋난다

- **관찰** (job `fwb-04`, 감독관 검증 완료): 규격 §3(127행)은 raw binding이 payload를
  "protobuf envelope 없이" 보낸다고 적는다. 그러나 `zlink-c`
  (`bench_zlink_client.cpp:14-16`의 `k_request_envelope`, `:130-140`의 `0x0a` + varint +
  payload)와 `zlink-dotnet`은 **JSON envelope 헤더와 protobuf로 인코딩한 `BenchPayload`를
  두 part로** 보낸다.
- **결정**: **구현이 맞고 규격 문장이 틀렸다.** formula 1이 `zlink-<lang>`을 `zlink-c`로
  나누므로 wire 모양이 다르면 서로 다른 실험을 나누게 된다. 규격을 구현에 맞춰 고친다.
- job `fwb-04`가 구현을 따른 것은 옳은 판단이다.

## FB-025 — `zlink-node` raw request가 window 4를 넘으면 붕괴한다

- **관찰** (job `fwb-04`, smoke 1024 B 2초, raw ROUTER↔ROUTER):

  | window | 처리량 | client 코어 | peak_in_flight |
  |---|---|---|---|
  | 1 | 10,507/s | 1.31 | 1 |
  | 4 | 34,466/s | 1.39 | 4 |
  | 16 | **1,294/s** | 1.00 | 16 |
  | 100 | **2,568/s** | 1.00 | 100 |

  window 100에서 평균 174 ms인데 p95는 1.16 ms, p99는 1.57 ms다. 대부분 1 ms에 끝나고
  소수가 수 초 멈추는 꼬리다. `peak_in_flight`가 설정값에 도달하고 abandoned가 0이므로
  **harness가 window를 못 채우는 FB-010 유형이 아니다.**
- **결정**: Node 행을 판정하기 전에 **harness·Node binding·Core 중 어디인지 분리한다.**
  분리 방법은 bench 밖의 최소 재현이다. `bindings/node`의 자체 perf가 같은 window에서
  같은 붕괴를 보이면 bench 밖의 문제이고, 보이지 않으면 bench를 먼저 의심한다.
- **성격이 정해지기 전까지 Node의 어떤 판정도 게재하지 않는다.**

## FB-026 — Node binding의 client socket wedge (bench 밖에서 재현됨)

- **판정** (job `fwb-04b`): FB-025의 붕괴는 **bench 밖에서 재현된다.** harness를 전혀
  import하지 않고 `@zlink-systems/zlink`만 쓰는 재현 코드
  (`framework/languages/node/bench/with-grpc/repro/`)가 같은 현상을 만든다. **harness는
  혐의를 벗었다.**
- **관찰** (ROUTER↔ROUTER, 1024 B, blocking-recv server):

  | window | 처리량 | 오류 | p50 | max |
  |---|---|---|---|---|
  | 8 | 48,491/s | **0** | 0.144 ms | 1.4 ms |
  | 16 | 3,353/s | 16 | 0.159 ms | 3001 ms |
  | 24 | 935/s | 24 | 0.201 ms | 3001 ms |
  | 100 | 1,134/s | 74 | 0.592 ms | 3001 ms |

  `max`가 정확히 request timeout이므로 느린 완료가 아니라 **만료되는 정지**다.
  p50은 끝까지 1 ms 미만이다. 정상 흐름과 멈춘 집합이 공존한다.
- **성격**:
  1. server pump 구현 문제가 아니다. blocking-recv server와 spin pump 둘 다에서 나온다.
  2. 제출 루프 모양 문제가 아니다. batch 모양에서도 나온다.
  3. **동시성 깊이 문제가 아니다.** 100개 동시 요청 one-shot은 1.1 ms에 100/100 완료된다.
     지속적인 부하가 있어야 발생한다.
  4. **client socket에 한정된 hard wedge다.** window 32에서 `completed`가 251 ms에
     12,600에 도달한 뒤 남은 2.8초 동안 **한 번도 전진하지 않고** `inFlight`가 32에 고정된다.
     그 socket이 멈춘 동안 **같은 server에 새로 붙인 client ROUTER는 1.5 ms에 100/100을
     완료한다.** server는 정상이고 멈춘 것은 client socket이다.
  5. 간헐적이다. 한 번은 141,481건을 처리한 뒤, 다른 때는 12,600건 뒤에 멈췄다.
- **처리**: 이 캠페인은 고치지 않는다. **0.18.0 후보이며 우선순위가 높다.** 완료가 영구히
  멈추는 것은 성능 특성이 아니라 정지 결함이다.

## FB-027 — Node는 0.80 판정을 게재하지 않는다. 그 사실이 Node의 결과다

- **관찰** (job `fwb-04b`): `zlink-node-request-window`의 event loop 사용률이 **window 8,
  즉 FB-026 정지가 발생하지 않는 구성에서도 1.000**이다. 같은 run에서 `grpc-node`는
  0.671~0.732로 포화가 아니다.
- **의미**: Node에서 ZLink client 경로는 **transport가 상한이 되기 전에 JS thread를 먼저
  채운다.** 규격 §5.1에 따라 그 셀은 포화로 표시되고 처리량 우열 판정에서 제외되므로,
  formula 1의 분자가 게재 조건을 만족하지 못한다.
- **결정** (2026-09-07, 감독관): **Node의 0.80 판정을 게재하지 않는다.** 이것을 실패나
  누락으로 적지 않는다. **"Node에서는 ZLink client 경로가 transport보다 먼저 JS thread를
  포화시킨다"가 Node에 대해 이 캠페인이 낸 결론이고, 판정 불가는 그 결론의 결과다.**
- 표는 그대로 싣는다. 처리량·지연·event loop 사용률·drain 시간은 정보를 담고 있다.
  게재하지 않는 것은 0.80 비율뿐이다.

## FB-028 — Node framework의 공개 protobuf codec은 bytes를 표현하지 못한다

- **관찰** (job `fwb-04b`, 감독관 검증 완료): `packages/framework-codec-protobuf`의
  `encodeDynamicValue`(`src/dynamic-value-wire.ts:34-58`)는 `boolean`·`number`·`string`·
  `object`만 처리한다. **Buffer·Uint8Array 분기가 없다.** Buffer는 `object`로 떨어져
  바이트마다 키 항목이 하나씩 생긴다.
- **측정**: 1024바이트 `bytes body`가 **20,412바이트(19.9배)** 로 인코딩되고 bytes가 아니라
  평범한 object로 디코딩된다.
- **영향**: 규격 §2가 payload를 protobuf `bytes body`의 크기로 고정하므로, Node framework
  행은 **규격이 정한 payload를 실을 수 없다.** 여섯 셀을 `unsupported`로 두고 사유를
  declaration gap으로 남긴다. 규격 §10.1이 C++·.NET relay에 대해 쓰는 방식과 같다.
- **판단**: 다른 payload로 바꿔 재지 않는다. 그러면 다른 실험을 재는 것이다.
  `src/internal.ts`의 비공개 경로로 우회하지 않는다(G4).
- **후속**: 0.18.0 후보. messaging framework의 공개 codec이 이진 payload를 실을 수 없는 것은
  bench의 문제가 아니라 제품의 제약이다.

## 감독관 수용 — `zlink-c` 기준선 재실행 생략

job `fwb-04b`가 Node pass에서 `zlink-c` 기준선을 다시 재지 않았고 그 사실을 먼저 알렸다.
근거는 그 값의 유일한 소비처가 formula 1인데 FB-027이 Node의 0.80 비율을 게재하지 않기로
정했으므로 게재되는 어떤 값도 바뀌지 않는다는 것이다. **타당하므로 수용한다.** 지시에서
벗어난 것을 묻히지 않고 먼저 보고한 처리가 옳다.

## FB-029 (정정됨) — handler 생성 실패가 삼켜진다. 메시지는 수락되고 버려지며 sender는 성공을 받는다

- **최초 기록은 틀렸다.** 2026-09-07 감독관이 "framework channel send가 무성 손실된다"로
  우선순위 0에 올렸으나, job `fwb-05`가 20분 상한 계측으로 스스로 반증했다. framework는
  send를 잃지 않는다. 배선을 고친 뒤 같은 셀이 **3,236 msg/s**를 낸다.
- **실제 원인**: harness가 `BenchServerMetrics`를 `beanFactory.registerSingleton(...)`으로
  등록했다. 인스턴스는 생기지만 **bean definition이 없다.**
  `ZLinkSpringHandlerFactory`는 생성자 의존성마다 `isPrototype()`을 묻는데 definition만
  그 질문에 답할 수 있어 handler를 생성하지 못했다. request handler는 의존성이 없어 살아남았고,
  그래서 request는 되고 send만 안 되는 모양이 나왔다.

  ```
  IllegalStateException: failed to construct Framework-owned handler: BenchCommandHandler
  Caused by: NoSuchBeanDefinitionException: No bean named 'benchMetrics' available
    at ZLinkSpringHandlerFactory$SpringActivation.create(...:163)
  ```

- **그러나 제품 쪽 결함은 남고, 더 정확해진다.** handler 생성이 실패했을 때 framework의 반응이
  문제다. 메시지를 **수락하고**, `received`·`admitted`·`dispatched`까지 **trace로 기록하고**,
  **버리고**, sender에게는 **성공을 돌려준다.** DEBUG 로그에도 `LOG_AND_DROP`에도 아무것도
  남지 않는다. 원인이 harness 배선이었다는 사실은 이 반응을 바꾸지 않는다. **사용자 코드의
  handler 생성이 운영에서 실패하면 같은 일이 일어난다.**
- **`dispatchSend`가 `whenComplete`를 성공 경로에서만 trace한다**는 것이 전부의 원인이다.
  이 결함을 찾는 데 그것만 잡으려고 작성한 `ZLinkHandlerFilter`가 필요했다.
- **처리**: 이 캠페인은 고치지 않는다. **0.18.0 후보 우선순위 0을 유지하되 제목을 바꾼다.**
  "send 무성 손실"이 아니라 "handler 생성 실패의 무성 수락·폐기"다.
- **교훈**: 감독관이 job의 최초 진단을 그대로 우선순위 0에 올렸다. job이 20분 계측으로
  뒤집었다. **진단은 계측 전까지 가설이다.**

## FB-030 — Java raw의 reply가 depth 16 이상에서 유실된다

- **관찰** (job `fwb-05`, binding만 쓰고 bench 코드를 import하지 않는 최소 재현):

  | outstanding | in-process echo server | bench raw server 대상 |
  |---|---|---|
  | 4 | 4/4 | 4/4 |
  | 16 | 16/16 | 0/16, 5/16, 10/16 (run마다) |
  | 100 | **43/100 (server는 101건 전부 echo)** | 0/100, 5/100 |

- **결정적 수치는 in-process 100건이다. server가 101건 reply를 모두 보냈고 client는 43건을
  완료했다.** reply가 생성되고 server의 submit과 client의 완료 사이에서 사라진다.
  reply 분기 자체는 정상이다(`hasToken=true parts=2`). 간헐적이며 고정 상한이 아니다.
- **Node FB-026보다 증거가 강하다.** Node는 멈춘 socket만 보여줬고, Java는 **reply가 나가고
  버려지는 것**을 보여준다.
- **처리**: 고치지 않는다. `request-window`를 Node와 같이 관측된 그대로 보고한다.
  0.18.0 후보. 재현 코드를 저장소에 남긴다.

## FB-031 (정정됨) — 완료 전달이 무너지는 지점과, 무엇이 독립 관측인가

- **최초 기록이 binding 수를 과다 계산했다.** 감독관이 Node·Java·Kotlin을 세 개의 독립
  binding으로 적었으나, job `fwb-06`이 정정했다. `bindings/kotlin`에는 native binding이
  없고 Java의 `systems.zlink:zlink` artifact를 Kotlin에서 쓰는 구조다(저장소 확인 완료 —
  `bindings/kotlin` 아래에 `samples`만 존재).

| 실행 | 관측 | 독립성 |
|---|---|---|
| C | depth 90.7 유지, 오류 0 | 독립 |
| `.NET` | depth 8에 묶임. 제출 비용이며 유실 없음(FB-016) | 독립 |
| Node | depth 8까지 정상, 16 이상 socket 정지(FB-026) | 독립 |
| Java | outstanding 2 이상 유실 시작, window 100에서 완료 0(FB-030) | 독립 |
| Kotlin | Java와 같은 서명 | **Java binding 재사용. 독립 관측 아님** |

- **Kotlin이 더하는 것**은 새 binding이 아니라 **다른 호출 형태에서도 같은 일이 일어난다는
  사실**이다. coroutine `await`와 blocking `get`은 다른 코드인데 둘 다 정지하고, ROUTER와
  DEALER 양쪽에서 나온다.
- **남는 결론**: 독립 관측은 C·`.NET`·Node·Java 네 개다. C만 깊이에서 멀쩡하고 나머지 셋이
  각각 다른 방식으로 무너진다. C가 depth 90에서 정상이므로 Core 공통 경로만의 문제로 단정할
  수 없다. C++가 이 대비의 마지막 데이터다(job `fwb-07`).

## FB-032 — Java의 포화 계측기는 `jvm_thread_cores`로 한다

- **관찰** (job `fwb-05`): `client_cores`(상한 20)는 발동하지 않을 뿐 아니라 **서로 다른 것을
  비교한다.**

  | 셀 | 프로세스 코어 | JVM 스레드 코어 | 비 JVM |
  |---|---|---|---|
  | `grpc-java` request-window | 3.08 | 3.00 | 0.09 |
  | `zlink-java` send-saturation | 2.17 | **0.15** | **2.03** |
  | `zlink-framework-java` send-saturation | 2.81 | 2.17 | 0.65 |

  `zlink-java`는 프로세스 CPU의 94%가 Core의 native I/O thread이고 user 코드를 실행하지 않는다.
  `grpc-java`는 3%다. 이 둘을 나누면 다른 종류의 양을 나누는 것이다.
- **결정** (2026-09-07, 감독관): **`jvm_thread_cores`(`ThreadMXBean`, 공개 API)를 harness가
  선언한 제출 병렬도 기준으로 쓴다.** 집계기가 계측기 이름으로 해석하므로 세 번째 계측기를
  받도록 집계기를 고친다. **Node의 FB-023과 같은 모양의 오류이며 같은 방식으로 고친다.**

## FB-033 — framework가 정지를 피하는 이유는 깊이에 도달하지 않기 때문이다

- **관찰** (job `fwb-05`): `zlink-framework-java` request-window가 설정 window 100에 대해
  `peak_in_flight` **10~11**, 실제 깊이 **약 4.5**, abandoned 0으로 동작한다. 같은 binding
  위의 `zlink-java` raw는 같은 설정에서 `peak_in_flight` 100, abandoned 100으로 전부 정지한다.
- **의미**: FB-030의 유실은 outstanding 2 이상에서 시작해 16 이상에서 전면화한다. framework는
  **자기 경로의 깊이 상한이 약 10이라 그 구간에 들어가지 않는다.** 상위 계층이 하위 계층보다
  견고해서가 아니라 덜 깊게 들어가기 때문이다.
- **활용**: FB-030의 원인을 좁히는 단서다. 같은 binding에서 깊이만 다른 두 경로가 갈리므로,
  원인은 socket 생성이나 연결 설정이 아니라 **동시 미완료 요청 수에 따라 달라지는 완료 전달
  경로**에 있다. 0.18.0 조사의 출발점으로 기록한다.
- framework의 깊이가 왜 10에서 멈추는지는 이 캠페인이 규명하지 않는다.

## FB-034 — `zlink-c` request-window @4096이 세 구간 연속 G5 미달이다. formula 1 @4096은 어느 언어도 게재할 수 없다

- **관찰**: 독립된 세 측정 구간에서 같은 행이 재현성 조건을 계속 넘지 못한다.

  | 측정 | `zlink-c` request-window @4096 G5 | 독립성 |
  |---|---|---|
  | Phase 0 `gated` | 75.7% | 독립 |
  | Phase 0 `gated2` | 25.7% | 독립 |
  | Phase 3 Java 구간 | 28.7% | 독립 |
  | Kotlin·C++ 구간 | 28.7% | **같은 Java 구간 값을 재사용. 독립 측정 아님** |

  **정정** (2026-09-07, job `fwb-07` 지적): 감독관이 "네 구간 연속"으로 적었으나 독립 측정은
  **세 번**이다. Kotlin과 C++ 구간이 Java 구간의 기준선을 재사용했다. 결론은 유지되지만
  근거 수는 셋이다.

- **구조적 결과**: 이 행은 formula 1(`zlink-<lang> / zlink-c`)의 **공유 분모**다. FB-011에 따라
  분모가 G5를 통과하지 못하면 판정을 게재할 수 없고, FB-005에 따라 두 payload 크기를 모두
  만족해야 통과다. 따라서 **@4096 분모가 안정되기 전까지 어떤 언어도 formula 1을 통과할 수
  없다.** 그 언어의 품질과 무관하다.
- **결정** (2026-09-07, 감독관): 이 사실을 통합 보고서의 결론에 넣는다. 판정 기준이 현재
  기준선 위에서 달성 불가능하다는 것은 언어별 결과와 별개인 캠페인 수준의 발견이다.
  기준선 안정화는 이 캠페인의 범위가 아니며 0.18.0 후보로 올린다.
- **주의**: 이것을 판정 기준을 완화할 근거로 쓰지 않는다. 분모를 안정시키는 것이 답이지
  임계값을 낮추는 것이 답이 아니다.

## FB-035 — FB-033의 범위를 좁힌다. 깊이 상한은 client 언어가 정하지 않는다

- **관찰** (job `fwb-06`): Kotlin framework의 request-window 여덟 셀이 깊이 **4.48~4.57**,
  `peak_in_flight` 10~12로 Java의 4.5 / 10~11과 일치한다.
- **정정된 해석** (job `fwb-06`이 감독관 지시보다 좁게 읽었고 그것이 옳다): Kotlin 행은
  **Java 행의 framework server를 재사용**하므로 server 쪽이 공유된다. 따라서 "두 독립 언어에서
  확인"이 아니다.
- **그래도 얻는 것**: Java의 `CompletionStage` 경로와 Kotlin의 suspend 경로는 서로 다른
  코드인데 **둘 다 4.5에서 멈춘다.** 즉 **깊이 상한을 정하는 것은 client API 계층이 아니다.**
  FB-033의 후보에서 client 쪽을 제거하고 공유 framework request 경로만 남긴다.
  server 쪽이라고 단정하지는 않는다. 두 client 모두 같은 framework core를 지난다.

## FB-036 — Kotlin 측정 기록

- 4 run × 18셀 = 72셀, 실패 0, 오염 0, 전 run rc=0.
- raw request-window 여덟 셀 전부 정지(`peak_in_flight` 100 / abandoned 100 / errors 100,
  DEALER 포함). 그 여덟 셀 밖의 오류는 네 run 전부에서 0이다. warmup 열 구간이 모두 0이므로
  정지는 active 구간에서 생긴 것이 아니라 처음부터 있었다.
- `grpc-kotlin`은 coroutine stub(`BenchServiceCoroutineStub`, `protoc-gen-grpc-kotlin` 1.4.1)을
  실제로 사용했다. blocking stub 대체가 없었으므로 규격 §8.1의 사유 기록이 필요 없다.
- 판정 네 건 전부 `unsupported`.
- G5는 Kotlin 16행 중 15행 통과. `zlink-framework-kotlin` send-saturation @1024이 12.8%로
  미달하며 원인은 규명하지 않았다.
- warmup 20초를 Kotlin 자체 데이터로 정당화했다. `grpc-kotlin` request-serial이 3.2배
  (1580 → 5004) 오르고 3구간(약 6초)부터 중앙값 ±10% 안이다.
- `zlink-c` request-window @4096이 **네 번째 구간 연속** G5 미달(28.7%). FB-034 강화.
  Java 구간의 기준선을 재사용했고 그 사실을 요약 §2.8에 명시했다. 두 구간 사이 변경은
  bench 코드·집계기·문서뿐이며 Core·binding·framework source는 바뀌지 않았다.

## FB-037 — 완료 전달 결함은 관리형 런타임 binding에 국한된다 (캠페인 핵심 결론)

- **판정** (job `fwb-07`, 독립 2회 재현): `zlink-cpp`가 **request window를 유실 없이 완전히
  지탱한다.**

  | 행 | 처리량 | 실제 깊이 | peak_in_flight | abandoned | 오류 |
  |---|---|---|---|---|---|
  | `zlink-cpp` request-window @1024 | 336,559/s | **99.85** | 100/100 | **0** | **0** |
  | `zlink-cpp` request-window @4096 | 274,512/s | **99.77** | 100/100 | **0** | **0** |
  | `grpc-cpp` request-window @1024 | 64,727/s | 99.89 | 100/100 | 0 | 0 |

- **다섯 실행을 나란히 놓으면 경계가 분명하다.**

  | 실행 | C API와의 거리 | 깊이 거동 |
  |---|---|---|
  | C | 직접 | depth 90.7, 오류 0 |
  | **C++** | **얇은 wrapper** | **depth 99.85, 오류 0** |
  | `.NET` | 관리형 | depth 8에 묶임(제출 비용, 유실 없음) |
  | Node | 관리형 | depth 8 초과 시 socket 정지 |
  | Java | 관리형 | outstanding 2 이상 유실, window 100에서 완료 0 |

- **결론**: C API를 직접 쓰는 두 실행은 모두 깊이를 지탱하고, 관리형 런타임 binding 셋은
  모두 무너진다. **이 증거로는 Core가 지목되지 않는다.** FB-026·FB-030·FB-016의 조사 범위가
  Core 공통 경로에서 **각 관리형 binding의 완료 전달 계층**으로 좁혀진다.
- **한계**: 이는 이 workload·이 조건에서의 관측이다. Core에 문제가 없다는 증명이 아니라,
  이 실험이 Core를 지목하지 않는다는 뜻이다. 세 binding이 각기 다른 방식으로 무너지므로
  하나의 공통 원인이라고 단정할 수도 없다.
- **활용**: 0.18.0의 세 항목(FB-026 Node 정지, FB-030 Java 유실, FB-016 .NET 제출 한계)에
  이 대비를 근거로 붙인다. 조사자는 Core가 아니라 binding의 완료 전달부터 본다.

## FB-038 — C++ 포화 계측기로 `submit_thread_cores`를 승인한다. 그리고 세 번 반복된 교훈

- **관찰** (job `fwb-07`): `zlink-cpp`의 제출 스레드가 window 100에서 **자기 상한의 0.955**인데
  프로세스 코어는 1.92로 읽힌다. `grpc-cpp`는 제출 0.695, 프로세스 0.698로 거의 같다.
  프로세스 코어로 판정하면 판정식이 나누는 두 행을 **다른 종류의 양으로 비교**하게 된다.
- **결정** (2026-09-07, 감독관): `submit_thread_cores`(harness 자신의 제출·drain 스레드 CPU,
  상한 1)를 **네 번째 계측기로 승인**한다. 집계기 whitelist에 추가하고 테스트를 더한다.
  `client_cores`는 관측값으로 함께 낸다. Phase 0 인수 fixture가 그대로 재현되어야 한다.
- **세 번 반복된 교훈**: Node(FB-023), Java(FB-032), C++(FB-038)에서 **매번 "당연해 보이는"
  프로세스 CPU가 틀린 계측기였다.** 이유가 언어마다 달랐다 — Node는 binding의 native I/O
  thread, Java는 GC·JIT thread, C++는 binding의 I/O thread. **ZLink client에서 프로세스 CPU는
  포화 계측기로 쓸 수 없다**는 것이 이제 언어 중립적 결론이다. 규격 §5.1이 언어마다 계측기를
  선언하게 한 것이 옳았고, 다섯 언어 중 넷이 프로세스 CPU가 아닌 것을 선언했다.

## FB-039 — C++ 판정 0.774는 게재하되 포화 제외선에서 0.00003 차이다

- **관찰** (job `fwb-07`): `zlink-cpp / zlink-c` @1024 = **0.774**, 기준 0.80 미달로 게재.
  이 캠페인의 두 번째이자 마지막 게재 판정이다(첫째는 `.NET`의 0.084).
- **반드시 함께 적을 것**: 이 셀의 포화 계측값 중앙값이 **0.94997**이고 제외 임계값은
  0.95다. **0.00003 차이로 제외를 면했다.** run 하나만 달랐어도 Node처럼 `unsupported`가
  됐을 값이다.
- **오독 금지**: 0.774를 "C++ binding이 C의 77%를 낸다"로 인용하지 않는다.
  `zlink-cpp`는 request-serial(8.97 대 8.29 KOPS)과 send-saturation(696.4 대 699.4,
  514.7 대 485.8 KMSG/s)에서 `zlink-c`와 같거나 낫다. **격차는 깊이에서만 나타나고,
  거기서 C++는 더 많이 물고 있으면서(99.9 대 86.9) 더 적게 처리한다.** 요청당 client
  비용이 더 크다는 뜻이다. coroutine frame 가설은 후보로만 기록하며 프로파일하지 않았다.

## FB-040 — 규격 §5.1의 언어별 계측기 표가 실제 선언과 어긋난다

- **관찰** (job `fwb-07`): 규격 §5.1 표는 `dotnet·java·kotlin·cpp`가 프로세스 코어를 쓴다고
  적는데, java·kotlin은 `jvm_thread_cores`(FB-032), cpp는 `submit_thread_cores`(FB-038)를
  선언한다. **표가 세 언어의 실제 선언과 모순된다.**
- **결정**: 규격 표를 실제 선언에 맞춘다. 계측기 이름과 상한을 언어마다 적고, `.NET`만
  프로세스 코어를 쓴다는 사실을 남긴다. 다섯 언어 중 넷이 프로세스 코어가 아닌 것을
  선언했다는 것이 FB-038의 언어 중립적 결론과 이어진다.

## FB-041 — Phase 6의 재측정 범위를 바꾼다. 전면 재측정 대신 통합 검증 구간 1회

- **계획 초안**: "구현 중에 얻은 값은 개발 중 값이므로 채택하지 않는다. 완성된 다섯 언어를
  같은 조건에서 3회씩 다시 잰다."
- **실제로 일어난 일**: 다섯 언어 모두 **구현이 끝난 뒤 전용 gated 구간**에서 측정됐다.
  ROUTER 3회 + DEALER 1회, 매 run 전 loadavg 게이트, 빌드는 구간 밖. 개발 중 값이 아니다.
  초안이 막으려던 위험은 이미 없다.
- **남는 진짜 위험**: 다섯 구간의 **commit이 다르다**(`d96e4b7031`·`9b47698915`·`dcded04dbe`·
  `129627f8a5`·`85e7a7613e`). 언어를 가로지르는 결론(FB-031·FB-037)이 구간을 가로지른
  비교 위에 서 있다. 각 job이 구간 사이 변경은 bench 코드·집계기·문서뿐이며 Core·binding·
  framework source는 바뀌지 않았다고 확인했으나, 그 확인은 논증이지 측정이 아니다.
- **결정** (2026-09-07, 감독관): 다섯 언어를 3회씩 다시 재지 않는다. 대신 **한 commit에서
  언어당 1회씩 도는 통합 검증 구간**을 한 번 실행한다.
  - 목적은 새 중앙값을 만드는 것이 아니라 **구간을 가로지른 비교가 성립하는지 시험**하는 것이다.
  - 각 언어의 검증 run이 그 언어의 기존 중앙값 대비 G5 허용 범위(±10%) 안이면 기존 데이터를
    그대로 보고서에 쓴다.
  - 벗어나는 언어가 있으면 **그 언어만** 다시 3회 잰다.
- **근거**: 전면 재측정은 약 5시간이고 같은 결과가 나올 가능성이 높다. 통합 검증은 약 40분에
  같은 질문에 직접 답한다. 시간을 아끼려는 것이 아니라 **재측정이 답하지 못하는 질문(구간 간
  비교 가능성)에 답하는 설계**를 고른 것이다.
- **보고서 의무**: 데이터가 다섯 구간에서 나왔다는 사실과 검증 결과를 §2 측정 조건에 명시한다.
  검증이 통과했더라도 "한 구간에서 잰 데이터"인 것처럼 쓰지 않는다.

## FB-042 — 관리형 binding의 공개 async request terminal은 admission 거절을 표면화하지 않는다

- **관찰** (job `fwb-09`, 계약 문서 직접 확인): Node `RequestSubmitOperation.submit()`,
  `.NET` `Async()`, Java `submit()` 모두 **backpressure를 내부에서 흡수한다.** WRITABLE
  token을 기다렸다가 같은 요청을 재제출하며 **호출자에게 거절을 돌려주지 않는다.**
- **send에는 있고 request에는 없다**: send terminal은 `TrySubmit()` → bool로 `Backpressured`를
  표면화한다. request terminal에는 세 binding 어디에도 대응 변형이 없다.
- **C만 관측 가능하다**: `zlink_request_part`에 `DONTWAIT`을 주면
  `ZLINK_SUBMIT_BACKPRESSURED`가 온다. C bench의 기존 루프가 이미 backpressure에서
  멈추는 이유이며 수정이 필요 없는 이유다.
- **결과**: "공개 terminal의 admission backpressure까지 제출한다"는 **C에서는 표현 가능하고
  관리형 네 언어에서는 표현 불가능하다.** 저장소 perf 정책(`PERF_MULTI_TEST_POLICY.md:164`)이
  요구하는 모델을 그 언어들의 공개 API로는 구현할 수 없다는 뜻이다.
- **처리**: 이 캠페인은 고치지 않는다. **0.18.0 후보.** 공개 API 공백이며, 성능 특성이 아니다.

## FB-043 — 저장소의 두 perf harness가 서로 다르고 하나는 정책을 어긴다

- **관찰** (job `fwb-09`): Node `perf_multi_socket_reqrep.ts`는 socket당 event-loop turn마다
  하나씩 제출하고 양보해 깊이가 제출·완료 속도 균형점에서 정해지게 둔다. Java
  `PerfMultiSocketReqRep`은 **socket당 미완료 요청을 정확히 1개로 제한**하며 그렇게 한다고
  주석에 적혀 있다. 후자는 `PERF_MULTI_TEST_POLICY.md:164`가 명시적으로 금지하는 1:1 직렬화다.
- **의미**: 같은 정책 아래의 두 harness가 다른 모델을 쓴다. Java의 REQREP perf 수치가
  inflight 1 왕복 수치일 수 있으며, 그렇다면 정책이 재려던 것과 다른 것을 재고 있다.
- **처리**: 이 캠페인의 범위 밖이다. **0.18.0 후보로 올린다.** 확인이 필요한 것은 Java perf의
  REQREP 값이 정책이 정한 모델로 측정된 것인지다.

## 감독관 결정 — 측정 루프는 (a), API 공백은 결과로 기록

job `fwb-09`이 두 선택지를 올렸다. (a) 연속 제출에 완료 pump 양보를 유일한 제한으로 두고
도달 깊이를 결과로 보고, (b) 관리형 terminal에 비차단 admission 변형이 없다는 것을 발견으로
두고 C·C++만 측정하며 나머지 넷을 공개 API 공백으로 막힘 처리.

**(a)를 채택한다.** 근거:

- (b)는 다섯 언어 중 둘만 측정한다. 이 작업은 사용자가 요청한 비교표를 얻으려는 것이고,
  방어 가능한 균일한 루프가 존재하는데 순수성 때문에 목표를 버리는 것은 맞바꿈이 나쁘다.
- 관리형 binding에도 backpressure는 **있다.** terminal 안에서 흡수될 뿐이다. 제출과 완료
  속도가 균형을 이루는 지점에서 깊이가 정해지는 것은 실제로 일어나는 일이다.
- 다만 **라벨을 정직하게 붙인다.** 네 행에 대해 이것은 "admission backpressure까지"가 아니라
  **"연속 제출 + 완료 pump 양보"** 다. 규격과 보고서에 그대로 적는다.

**FB-042를 결과로 싣는다.** 루프를 어떻게 짜든 그 공백은 사실이고, 이 측정이 그것을 드러냈다.
§5.2의 규칙(abandoned가 0이 아닌 셀은 비율 비교에서 제외)은 그대로 적용한다. 관리형 행이
깨끗한 셀을 내면 표가 성립하고, 내지 못하면 그것이 답이며 그대로 보고한다.

## FB-044 — 고정 window가 C harness의 readiness 결함을 가리고 있었다

- **관찰** (job `fwb-09`): 제한 없는 제출로 바꾸자 처음에는 전면 붕괴로 보였다.
  14,110건 admitted, 완료 0, 같은 수치로 재현. **Core 결함이 아니었다.**
- **원인**: C client에 readiness 탐침이 없고 500 ms sleep만 있었다. 제한 없는 제출기가
  경로가 서기 전에 약 14,000건을 쏟아붓고 전부 만료됐다.
- **고정 window 100이 이 결함을 가리고 있었다.** 처음 100건은 경로가 올라올 만큼 느리게
  나가기 때문이다. 규격이 이미 요구하고 C++ client에는 이미 있던 bounded probe를 넣었다.
- **수정 후**: 992,134건 완료, 오류 0, 깊이 557, 496.0 KOPS. 다른 run은 깊이 14,111에
  오류 0이었다. **14,111은 고장난 상태가 아니라 도달한 깊이였다.**
- **의미**: backpressure 모델이 고정 window 모델보다 나은 이유가 하나 더 생겼다. 깊이를
  강제하지 않으면 harness의 준비 상태 결함이 드러난다. 고정 window는 그것을 조용히 흡수한다.
- 남은 약점은 runner의 1초 server 대기다. probe가 `route ready=false`를 보고하므로 나쁜 셀이
  조용히 지나가지 않고 보인다.

### 부수 관측 — C의 backpressure 깊이

고정 window 100에서 C의 실제 깊이는 86.9~90.7이었다. 제한을 없애자 **557과 14,111**에
오류 0으로 도달한다. 즉 window 100은 C에게 상한이 아니라 **제약**이었다. 이 값들은
잠금 없는 2초 smoke이며 측정이 아니다. 정식 구간에서 다시 확인한다.

## FB-045 — 2차 캠페인 규격 개정(S0): server-driven 모델과 새 포트 대역 (2026-09-09, 커밋 `bc6c54d37a`)

- **결정(사용자)**: 측정 대상은 server A→server B 메시징이고 HTTP 호출은 trigger일 뿐이다. 포트는
  자유롭게 정한다. Kotlin은 보조 셀(`grpc-kotlin`, `zlink-framework-kotlin`의 `request-window @1024`)만
  잰다. 표 단위는 request `KOPS`, send `KMSG/s`, 5초 active 평균(3-run 중앙값)과 p95/p99.
- **규격 반영**: §3 실행 조건(A/B 한 쌍, runner는 부하를 만들지 않음), §4 원본에 `role`·`trigger`·
  `streams`·`target_stats`, 표 머리 `Source/Target`(metric 이름 `client_*`/`server_*`는 집계기 호환을
  위해 유지), §7.2에 "zlink-c 분모는 client-driven 값"임을 적고 비율은 부록으로, §9 언어당 20개
  대역(5200+, C 6200+), §10 신설(역할·trigger 계약은 perf README §4.2·§5.1·§16 재사용·logical
  stream과 패턴 대응·셀 순서·Kotlin 보조 셀).
- **C 기준 bench**는 client-driven 그대로 둔다. 기준값으로 쓰는 것은 요청당 비용이지 부하 생성
  위치가 아니기 때문이다.

## FB-046 — bench 통합 위치와 규격 문서의 사이트 노출 (2026-09-09, 커밋 `c57d6c26f2`·`22c8dfe08d`)

- 규격은 `framework/bench/grpc/README.{ko,en}.md`가 정본이고, 옛 경로
  `framework/doc/framework/common/bench/with-grpc-local.{ko,en}.md`는 symlink로 남겨 사이트(`common`
  symlink)와 기존 링크가 계속 열린다. 언어별 문서·비교 보고서는 `framework/bench/grpc/doc/`에 두고
  사이트 nav는 S5에서 붙인다.
- 1차 계획·decisions·perf 기록의 옛 경로는 당시 기록이므로 고치지 않는다.
- **사고 기록**: fwb2-01 job이 `git mv`로 stage해 둔 rename 124개가 감독자의 규격 커밋
  `bc6c54d37a`에 함께 들어갔다(내용 변경 없는 순수 rename). history는 두고 `c57d6c26f2`의 메시지에
  적었다. 이후 감독자 커밋은 `git commit -- <경로>`로 지정 경로만 커밋한다.

## FB-047 — .NET S1 3-run: framework 계층이 raw의 13~18%, request-backpressure는 오류·수십 초 지연 (2026-09-09, 원본 `log/dotnet/with_grpc_dotnet_s1{b_run2,c_run1,d_run3}_*`)

집계기 중앙값(request KOPS, send KMSG/s; `--judgement-pattern request-window`):

| 셀 @1024 | grpc-dotnet | zlink-dotnet | zlink-framework-dotnet | framework/raw |
|---|---:|---:|---:|---:|
| request-serial | 5.49 | 6.19 | 1.51 | 0.24 |
| request-window | 117.40 | 85.47 | 12.21 | 0.14 |
| request-backpressure | 44.95 | 32.29 | 0.71 (errors 2,511, mean 26.7 s, p99 53.8 s) | — |
| send-saturation | 29.28 | 682.82 | 87.61 (mean 1.5 s, source 1.08 GB) | 0.13 |

- formula 2(`zlink-framework-dotnet / zlink-dotnet ≥ 0.80`)는 모든 셀에서 실패한다. formula 1
  (`zlink-dotnet / zlink-c`)도 window @1024에서 85.5/488.4 = 0.17로 실패(binding 캠페인 범위).
- 조용한 창에서 다시 잰 3-run(`results/s1-dotnet-c-2026-09-09.md`): G5 실패는 24행 중 4행
  (framework backpressure 1024/4096, framework send 4096, grpc serial 1024)뿐이다. 중앙값은 위 표와
  같은 크기(window @1024: grpc 139.6 / raw 98.9 / framework 12.3 KOPS). **formula 2 =
  0.124(1024)·0.127(4096), published·fail.** formula 1은 분모 zlink-c window가 G5 12.0%/11.8%로
  실패해 unsupported(FB-048의 Core I/O 배치가 1024에도 미침).
- framework 계층의 배율(0.12~0.24)과 backpressure의 오류·지연은 부하로 설명되지 않는 크기라
  제품 판정 대상으로 기록한다.
- request-backpressure의 framework 오류 2,511건은 admission 거절이 request terminal로 표면화되는
  경로(FB-042)에서 나온다. 규격대로 오류 셀은 처리량 판정에 쓰지 않는다(§5.2).
- 위치: framework .NET runtime. 1.0 묶음의 성능 항목으로 넘긴다(측정은 벤치가, 수정은 제품이).

## FB-048 — `zlink-c` 기준선 4096B의 두 모드는 Core의 연결별 I/O 스레드 배치 때문이다 (2026-09-09, fwb2-05)

- 관측: request-window·backpressure @4096이 run마다 ~420 vs ~263 KOPS 두 모드로 갈린다(G5 58.9%).
  1024B는 안정. 디버거 진단으로 낮은 모드에서 request socket의 application session·connecter·
  completion session이 같은 I/O 스레드에 배치됨을 확인(`ctx_io_thread_registry.cpp:96`의
  round-robin cursor를 application·connecter·completion이 함께 소비, 부하·기존 배치 미고려).
- 판정: harness 결함 아님(Submitted=Completed, Errors 0, window 100 도달), 외부 부하는 보조 요인.
  **Core(0.17.5)의 배치 재현성 문제.** 수정은 Core 소유이며 1.0 묶음의 Core 항목으로 넘긴다
  (임시 connecter가 이미 선택된 session I/O 스레드를 재사용하는 안을 후속 검토).
- 결과: formula 1의 4096B 분모는 게재 불가. 1024B 분모(G5 6.2%)만 쓴다. 보고서 §7.2 부록에
  이 사실을 적는다. harness·측정 조건은 바꾸지 않았다.

## FB-049 — Node raw `request-window @1024`에서 completion 100건 유실(binding 0.17.6) (2026-09-09, fwb2-04)

- 관측: source 완료 220, 오류(30초 timeout) 100, target 수신 320. C·C++·.NET의 같은 깊이 100은
  오류 0. 1차 캠페인 FB-026(0.17.5에서 수정됐다고 기록)이 published 0.17.6 smoke에서 재현됐다.
- 소유: Node 관리형 binding의 completion owner(`completion_owner.ts` `ensureRuntimeWatch`/
  `runtimeWake` 경계가 첫 조사 대상). Core 공통 경로보다 Node binding을 가리킨다. 사용자 결정에
  따라 중간 binding 릴리스는 없으므로 1.0 묶음의 bindings 항목으로 넘기고, 벤치는 그 셀을
  오류 셀로 기록한다(runner count 대조는 .NET과 같은 §5.2 불변식으로 통일).
- Node framework 행: `framework-codec-protobuf`가 protobuf bytes를 보존하지 못해 전 셀
  `unsupported`(run 원본 `unsupported.json`). 제품 결함, 별도 작업(사용자 결정).

## FB-050 — Java raw `request-window @1024`에서 reply completion 100건 전부 유실(binding 0.17.6) (2026-09-09, fwb2-06)

- 관측: 1차 `repro/`(ROUTER↔ROUTER batch, per-iteration `Received.close()`)를 published 0.17.6에서
  다시 돌리면 `outstanding=100 done=0/100`, 서버는 101건 수신·reply submit 완료. FB-026이 0.17.5에서
  수정됐다고 기록했으나 재현된다(Node의 FB-049와 같은 계열, Java는 전부 유실).
- 소유: Java binding의 caller-owned `Received` 수명 경계와 Core ROUTER reply lane
  (`Received.java:621-642, 786-840`, `NativeRouterSocket.java:49-75`; 스펙 07-router §291-303,
  321-324, 476-480). .NET은 reply context를 별도 객체로 capture해 같은 셀이 통과한다.
- 결정: 계획 §9의 중단 조건 — Java raw `request-window` 3-run과 그 행의 판정은 결함 수정 전
  진행하지 않는다. 수정은 1.0 묶음의 bindings 항목(FB-049와 함께).
- 부수 관측(smoke 1-run, 판정 아님): `zlink-framework-java request-serial` 309 ops/s vs raw 5,598,
  framework window 1,861 vs `grpc-kotlin` 64,433. Java framework 계층도 .NET(FB-047)과 같은 계열의
  배율 문제로 보이며 3-run 뒤 판정한다.

## FB-051 — 2차 send-saturation의 `KMSG/s` 분자는 settle 뒤 B의 active-header 수신 수다; drain 열을 함께 읽는다 (2026-09-09, fwb2-07 지적)

- 관측: fwb2-07이 집계기 `readers.py:606·615`가 source의 `server_received_at_close`를 settle 뒤
  `target_stats.received`로 덮고 send 처리량을 그 값/`durationMs`로 다시 계산한다고 지적했다.
  C++ framework send smoke(2초)에서 active 경계 2,204건, settle 뒤 8,918건, drain 5.5초 —
  원본 `RESULT` 1,102 msg/s가 표에서는 4,459 msg/s가 된다. FB-013은 1차 모델(client가 server
  snapshot을 직접 읽던 구성)에서 "active window가 닫히는 시점"의 snapshot을 요구했다.
- 판정: **집계기는 규격대로다.** 2차 규격 §6은 B가 header의 phase로 active 메시지를 세고, §10.4
  7단계는 settle 뒤 runner가 B stats를 `target_stats`에 합친다고 정한다. .NET·Node·Java runner
  (`dotnet/run_local.sh:191`, `node/run_local.sh:179`, `java/runner_common.sh:113-118`)와 집계기
  모두 같은 값을 쓰며 S1 published 표(FB-047)도 이 기준이다. 2차 모델에서 A는 B의 수신 수를
  알 수 없으므로 A의 "경계값"은 A 자신의 완료 관측일 뿐이다. C++ source가 남기는
  `completed_at_close`는 진단 필드로 유지한다.
- 그러나 FB-013의 우려(분모가 active 길이인데 분자에 drain 중 수신이 들어간다)는 그대로
  유효하다. **결정**: 표의 `KMSG/s`는 규격 값(active-header 수신 / `durationMs`)으로 싣고,
  결과 문서(언어별 §5·S5 비교 보고서)의 send 행에는 `drain ms`를 반드시 같은 표에 두며,
  drain이 active의 10%를 넘는 행은 `received / (durationMs + drainMs)`의 **소비율**을 각주로
  함께 적는다. 규격·집계기·runner는 바꾸지 않는다.

## FB-052 — C++ framework 계층: request-serial 0.55 KOPS(raw의 7%), window 1.05 KOPS — ms 단위 pump 의심 (2026-09-09, fwb2-07 smoke, 판정 아님)

- 관측(1-run 2초 smoke @1024): `zlink-framework-cpp` request-serial 555 ops/s(mean 1.80 ms)
  vs `zlink-cpp` 7,605(0.131 ms)·`grpc-cpp` 13,830; request-window(100) 1,052 ops/s(mean 92 ms),
  send-saturation 1,102 msg/s(active 경계) / drain 5.5초. .NET(FB-047)의 0.12~0.24, Java(FB-050
  부수)의 309 vs 5,598과 같은 계열이며 C++이 가장 낮다.
- 벤치 쪽 요인 검토: A는 public `task_t::await_ready()`를 `yield()`로 polling한다(driver 한
  thread). serial에서 in-flight 1건이 1.8 ms 걸리는 것은 polling 비용(µs)으로 설명되지 않는다.
  window 100에서 처리량이 2배에 그치고 지연이 92 ms로 늘어난 것은 요청당 ~1 ms의 직렬화가
  있다는 뜻이다.
- 제품 쪽 후보(fwb2-07은 조사하지 않음, 감독자 grep):
  `mesh_node_host_service.cpp:2476`(1 ms 단위 wait), `mesh_node_runtime.cpp:3283`·`:3799`
  (`co_await delay(1ms)` 루프), `route_mesh_runtime_service.cpp:482`(10 ms sleep).
- 결정: 3-run(S3) 값으로 판정하되, 벤치 결함으로 보지 않는다. 원인 조사는 1.0 묶음의 framework
  C++ 성능 항목(FB-047과 같은 항목)으로 넘긴다.

## FB-053 — Framework C++ HTTP host: listener bind 실패가 start 결과로 전달되지 않고 프로세스가 abort된다 (2026-09-09, fwb2-07 발견, 감독자 확인)

- 관측: raw A의 실제 포트 충돌에서 프로세스 abort. `http_listener.cpp:149`는 bind/listen 실패를
  `std::runtime_error`로 throw하고, `http_host_service_t::start`(`:515-526`)는 endpoint마다
  thread를 띄워 `run()`을 호출한 뒤 즉시 success를 돌려준다. thread 안의 예외는 잡히지 않아
  `std::terminate`. `app.is_ready()`가 bind 완료를 보장하지도 않는다.
- 소유: framework C++ runtime(HTTP hosting). 벤치는 HTTP 응답 readiness로 우회했다(규격 §10.4
  2·3단계가 요구하는 대기와 같다).
- 결정: 제품 결함. 1.0 묶음의 framework C++ 항목. 다른 언어의 HTTP host는 bind 실패를 start
  실패로 돌려주는지 1.0 준비 때 함께 확인한다.

## FB-054 — C++ framework: send-saturation warmup flood 뒤 RouteMesh send target이 사라져 active 전부 실패 (2026-09-09, 감독자 smoke, FB-012와 같은 계열)

- 관측: `zlink-framework-cpp send-saturation @1024`에서 warmup 5초는 ~4,400 msg/s로 정상
  (B `anyPhaseMessages` 17,972)인데, active 첫 send부터 모두 `RouteMesh channel send target was
  not found`(source 오류 17,500~18,254건, B 수신 0). 5회 연속 재현. fwb2-07의 원 binary와 원 runner
  (`3ad4d048c9`)로도 재현되므로 감독자의 runner 수정(`FB-054` 직전 커밋)과 무관하다. fwb2-07의
  smoke 1회 통과(8,918건)는 우연이었다.
- 소유: framework C++ runtime(RouteMesh peer 연결 유지). .NET 1차의 FB-012(saturation flood 뒤
  route가 영구히 끊김)와 같은 계열이며 C++은 warmup flood만으로 끊긴다.
- 결정: 제품 결함. 벤치는 그 셀을 오류 셀로 기록하고(판정 제외) run을 계속한다. 1.0 묶음의
  framework C++ 항목(FB-052·FB-053과 함께). 원본: `/tmp/zlink-claude-fwb2-s2/smoke-send{1,2,3}`,
  `smoke-astra-bin`, `smoke-old-runner`.
- 부수: 감독자의 첫 C++ 3-run(`s3q_run1`)은 framework request-backpressure 셀의 오류 3,457건에서
  runner가 중단돼 21/24 셀만 남았다(runner 결함, 수정 커밋 참조). 같은 셀이 수정 뒤 smoke에서는
  오류 0이었으므로 그 오류는 부하(감독자의 Node 테스트가 겹침)로 본다. 3-run은 `s3r_run{1,2,3}`으로
  다시 낸다.

## FB-055 — 3-run 중 드러난 runner 결함 3건: 오류·abandoned 셀에서 run 중단(C++·Java), load gate 즉시 실패(C++) (2026-09-09, 감독자 수정)

- C++ source는 오류/abandoned가 있으면 phase를 `failed`로 보고했고 runner는 거기서 run을 끝냈다
  (`s3q_run1` 21/24셀). Java source는 warmup drain 상한 뒤 남은 operation을 `IllegalStateException`으로
  던졌다(`grpc-java request-backpressure @1024`: warmup 20초에 3.3M 제출, 1.06M in-flight 잔류 →
  `s2q_run1` 중단). C++ runner의 load gate(2.0)는 이전 셀의 loadavg 잔상 때문에 다음 run을 시작 즉시
  실패시켰다(`s3r_run2`·`s3r_run3`).
- 규격 §5.2와 .NET source(`Program.cs:239-247`)의 규칙: abandoned는 기록하는 관측값이고 셀은 판정에서
  빠진다; 예외·readiness timeout만 실패다. C++·Java source를 이 규칙으로 맞췄다(Java는
  `warmup_abandoned` 필드로 기록). runner는 settle 상한 도달을 기록하고 다음 셀(새 process 쌍)로 간다.
  load gate는 값을 낮추지 않고 상한 600초 안에서 기다린다.
- 부수 관측(판정 아님): grpc-java future stub은 상한 없는 제출을 그대로 받아 in-flight가 백만 단위로
  쌓인다(.NET gRPC는 flow control로 21 ms 지연에서 멈춤). request-backpressure의 "도달 깊이"가
  구현마다 이렇게 다르다는 것이 이 패턴의 결과다.
- 재측정: C++ `s3s_run{2,3}`, Java `s2r_run{1,2,3}`(rebuild 티켓 뒤). Node `s2q_run{1,2,3}`·Kotlin은
  영향 없음.
- 추가(같은 날): C++ `s3s_run3`은 첫 셀에서 source의 HTTP listener bind 실패(`Address already in
  use`, FB-053 경로)로 abort. 이 머신의 `ip_local_port_range`가 `1024 65535`라 다른 프로세스의
  outbound 소켓이 고정 포트를 ephemeral로 잡는다(runner의 LISTEN preflight로는 못 잡음). 환경 조치:
  `sysctl net.ipv4.ip_local_reserved_ports=5200-5299,6200-6219`(측정 세션마다). 재측정 `s3t_run3`.

## FB-056 — C++ framework 병목 진단(P1, fwperf-cpp): 1 ms sleep이 아니라 요청당 동기 state-lane 왕복과 record당 끝나는 dispatch 회차; FB-054는 liveness probe가 application FIFO 뒤에 놓여 15초 만료 (2026-09-10, Issue #7·#8)

- 경로 계측(1-run, 2초): route API 전체 2,024 µs 중 encode→raw request 진입 245 µs(state-lane 왕복·operation 등록),
  raw request→B receive 462 µs, B receive→application queue 407 µs(mesh dispatch의 raw owner/mailbox/host 관리 작업),
  reply→A native scope 종료 320 µs, native 종료→route API 종료 384 µs(registry completion dispatcher·host mailbox).
  application queue 대기는 18 µs, handler 118 µs — 즉 병목은 handler가 아니라 **hop 사이의 관리 작업과 동기 handoff**.
- B의 dispatch 회차(`public_host_runtime.cpp:5656-5710`, `mesh_node_host_service.cpp:2203-2430`)는 application
  record **한 건**마다 끝나며 회차당 평균 873 µs(`tick_liveness` 108, `pump_one` 267, `dispatch_user_spot_operations`
  183 µs 등)다. window 100에서 B는 2초에 1,895회차·1,894건 수신 → 요청들은 겹쳐 있지만 단일 ingress가 1건/회차로
  소비해 ~1.06 ms/op. idle sleep(100 ms 상한 poll, 1 ms 종료 대기, 10 ms claim 펌프)은 active 경로가 아님(기각).
- FB-054 원인: liveness probe·ACK가 일반 application record와 같은 FIFO에 있어 warmup backlog(약 1.6만 건) 뒤에
  놓인 probe가 15초 deadline 안에 처리되지 못하고 `service_liveness_registry.cpp:103`이 peer를 제거 → topology
  select가 not_found. 스펙(05-transport-liveness §3-4)은 일반 메시지로 deadline을 연장하지 않는다고 정하므로, 수정은
  "probe/ACK를 application backlog와 분리해 처리" 쪽이 스펙과 맞는다.
- protobuf codec bridge는 왕복 최소 8회 payload 복사(sub-µs 수준, ms 병목 아님) — 후속.
- 실험: generic state-lane inline drain은 serial 0.64배 악화·window 1.58배·count 불일치로 기각(C). 제품 수정 없음.
- 결정(감독자): P2 승인 범위 — (1) B dispatch 회차의 관리 작업(liveness tick·spot 작업 등)을 record마다가 아니라
  회차/시간 단위로 상각하고, permit 예산 안에서 여러 application record를 한 회차에 소비, (2) request submit·completion
  경로의 `.run().get()` 동기 왕복 합치기, (3) liveness probe/ACK를 application FIFO 앞에서 처리(FB-054). codec 복사는
  뒤로. 브랜치 `framework-cpp/7-dispatch-turn-cost`(worktree zlink-fwperf-cpp), job `fwperf-cpp-p2`(astra).
  보고서: `.artifacts/codex/fwperf-cpp/summary.md`.
- **P2 1차 결과(02:20)**: 관리 작업 상각(819e2185d2) → window 0.014(×2.4), send 오류 0·count 일치, serial 불변. 0.90 미달.
  astra BLOCKER "control record 우선 처리는 04장 §3과 충돌" → 스펙 08-messaging-hot-path §4.2가 정리: 별도 queue 없이
  claim 즉시(I3) 처리. 2차는 스펙 08의 단계 모양으로 재개(Issue #7).

## FB-057 — .NET framework 병목 진단(P1, fwperf-dotnet): 단일 receive loop의 1건 읽기(spec은 64 batch)·단일 pump의 1 claim·요청별 cold Task+supervisor+DI scope, codec 전체 복사 (2026-09-10, Issue #5·#19)

- serial p50 968 µs: source encode→target outer decode 333 µs(service-wire envelope 생성·multipart 전체 복사·단일 receive
  loop), reply submit→source completion 227 µs, mailbox→pump claim 122 µs, handler 38 µs, codec 각 40 µs. 100 ms
  PollInterval은 idle 상한이라 정상 요청 경로가 아님(기각).
- 부하: `ZLinkManagedMeshNode.cs:4991-4994`가 Application job queue가 있으면 한 건만 `Recv`(spec 04-application-job-queue
  §3·§4는 회전당 최대 64건), `ZLinkMeshDispatchPump.cs:313-321`이 claim 1건, `ZLinkRuntimeTaskRunner.cs:106-168`이
  요청마다 cold `Task<Task>`+supervisor 등록 → window 100에서 socket 앞 7.45 ms·mailbox 앞 1.54 ms 대기(동시성은
  실제로 100). send는 소비 33 KMSG/s(제출 111)로 drain 4.7 s. 규격 published 값(111 KMSG/s)과 소비율 차이는 FB-051.
- codec: `ZLinkApplicationPayloadEnvelopeCodec.cs:166-183, 284-323, 357-370`이 payload 전체를 새 byte[]로 합치고 수신이
  part별 `Message.From` 재생성 — payload ownership spec(추가 복사 0) 위반.
- backpressure 오류 2,511건: `ZLinkRequestFailureMapper.cs:149-168`이 `Backpressured` submit을 DeadlineExceeded로 —
  binding/Core 정상 terminal인지 framework가 중간 admission을 terminal로 바꾼 것인지 repro로 분리 필요(Issue #19).
- 실험(2초 smoke, framework/raw): 기준 serial 0.171·window 0.037·send 0.157; socket batch 64 단독·PollOut 제거·inline
  dispatch·Task.Yield 제거 모두 0.90 미달(각 단독 효과 없음).
- 결정(감독자): P2 승인 — (1) receive 64 batch + pump 다중 claim + persistent worker batch 제출을 하나의 bounded drain
  규칙으로(규칙 3→1, permit·HWM·timeout 불변), (2) codec 전체 복사 제거, (3) DI fast path는 잔여 격차 시, (4) #19
  backpressure 분리 repro·framework 측이면 수정. C++ FB-056과 같은 구조(1건/회차 ingress). 브랜치
  `framework-dotnet/5-dispatch-batch`(worktree zlink-5-dispatch-batch), job `fwperf-dotnet-p2`(sol).
  보고서 `.artifacts/codex/fwperf-dotnet/summary.md`.

## FB-058 — Java framework 병목 진단(P1, fwperf-java): 1 ms park 폴링 수신 + 요청당 state-lane 동기 park 5회 + permit 전 receive·mailbox 3회 복사·1건 claim + executor hop (2026-09-10, Issue #6)

- hot path 순서 규칙 7개(idle sleep, service state lane, post-receive mailbox, 1-record application lane, Channel gate,
  handler executor, call-time selector). 확정: `ZLinkJavaRawServicePort.receive:239-246`+`ZLinkJavaRawMeshNode.startPump:4241-4273`이
  `waitForReadable(ZERO)` 뒤 `parkNanos(1 ms)`로 폴링(spec 04 §3 위반; wrapper가 `POLLCOMPLETION` wake를 readable로 안 봐
  completion-only wake도 1 ms park로 이어짐). polling-wait.patch 단독으로 serial 377→562 ops/s(+49%), RTT −0.87 ms.
- source 요청당 동기 join park 5회(topology `peers()` 2회 60 µs, liveness 51 µs, WRR 79 µs, port request 129 µs ≈379 µs);
  target은 permit 전에 receive해 mailbox에 full copy 3회 뒤 1건 claim(`dispatch:4391-4707`, `drainApplicationMailbox:7348-7417`);
  Channel serial queue→handler executor hop. send는 admission 11.6 KMSG/s vs target 소비 ~5 KMSG/s로 drain 2.6 s.
- window 100: Little's law로 평균 in-flight 4.6건 — source submit thread의 동기 hot path가 depth를 제한.
- 실험 최고치(polling+동기 owner+fusion): serial 0.186, send 0.025 — 단순 수정으로 0.90 불가.
- 결정(감독자): P2 승인 — (1) blocking readiness+permit-before-receive+bounded batch 64를 하나의 ingress owner로(Node·C++ 구조),
  mailbox copy 단계 제거, (2) selector 사전 준비(변경 시점)로 state-lane park 제거, (3) Message lifetime 단일 소유,
  (4) executor hop·completion chain은 잔여 시. 규칙 7→3 이하. 브랜치 `framework-java/6-mesh-ingress`, job `fwperf-java-p2`(sol).
  보고서 `.artifacts/codex/fwperf-java/summary.md`.

## 범위 밖으로 확인하고 미룬 항목

| 항목 | 처리 |
|---|---|
| 규격 §7.4의 단위 불일치 (C report는 KOPS, `RESULT` 라인은 초당 완료 수) | Phase 1 공용 집계기가 정규화한다. 집계기 도입 뒤 §7.4를 삭제한다 |
| C report 표의 열 구성이 규격 §4와 다름 (`Submitted`·`Completed`·`Errors` 등 추가 열) | Phase 1 공용 집계기가 흡수한다 |
| `bindings/c/bench/BENCH_POLICY.md`와 FB-001의 관계 — `with_grpc`의 DEALER 셀을 없앨지 병행할지 | Phase 0(job `fwb-02`)이 정책 문서를 읽고 판단해 보고한다 |

## job 기록

| job | Phase | 모델 | 상태 | 결과 |
|---|---|---|---|---|
| `fwb-02` | 0 | opus | 1차 완료·커밋 `7ecb81a461` | ROUTER 전환, harness 결함 4건 수정, FB-008 구현, gated pass 8/8. 판정은 FB-010·FB-011로 보류 |
| `fwb-04` | 2 | opus | 부분 완료·커밋 (측정 없음) | grpc-node·raw ROUTER 구현, framework 미구현. FB-023~025 발견 |
| `fwb-03` | 1 | opus | 완료 | 공용 집계기 `framework/bench/tools/`, 28 테스트. Phase 0 재현 확인, FB-019~022 발견 |
| `fwb-02b` | 0 | opus | 완료·커밋 `c67d677832` | FB-010 판정(b), FB-013 정정, gated2 8/8. ROUTER 3회 18/18 clean |
| `fwb-01` | 1(문서) | opus | 완료·커밋 `146db4da4c` | 규격 5언어 중립화(ko 338행·en 359행), FB-001~003 반영. 고정값·RPC 미변경 확인. FB-004·FB-005 추가 지시 |
| `fwb2-01` | S-1 | sol | 완료·커밋 `bc6c54d37a`(rename)·`c57d6c26f2` | `framework/bench/grpc/` 통합, 공통 `bench.proto` 하나, 5언어 빌드·집계기 50 테스트·언어별 1셀 smoke 티켓 통과 |
| `fwb2-02` | S1 | sol | 완료·커밋 `eb24d66003`(+감독자 `59521bf2f4` trigger 필드 확정) | 집계기 S2S 스키마·(runId, cellId) 병합·incomplete·Source/Target·KOPS/KMSG/s·`doc-table`, 테스트 60 |
| `fwb2-03` | S1 | sol | 완료·커밋 `d59e8a00e8` | .NET A/B runner, ServerSupport 재사용, 5셀 smoke rc=0. 3-run은 감독자 티켓(claude-fwb2-s1) |
| `fwb2-04` | S2 | sol | 완료·커밋 `b2aeda9f9d` | Node A/B runner, unsupported manifest, raw window 결함 재현(FB-049) |
| `fwb2-06` | S2 | sol | 완료·커밋 `f8b4fa98dd` | Java A/B runner + Kotlin 보조 2셀, raw window reply 유실 재현(FB-050) |
| `fwb2-07` | S3 | astra | 완료·커밋 `3ad4d048c9` | C++ A/B runner + `zlink-framework-cpp`(RouteMesh typed protobuf), 6셀 smoke rc=0. FB-051(집계기 지적 기각)·FB-052·FB-053 |
| `fwb2-05` | S4 | astra | 완료·커밋 `7786eec28a`(보고서만) | zlink-c 4096B 두 모드 = Core I/O 배치(FB-048), 4096 분모 게재 불가 |

## FB-059 — Java framework P2 1차: 수신 폴링·중복 hop 제거로 serial ×3.9, 그래도 0.90 미달. 남은 비용은 요청마다 다시 만드는 envelope header (2026-09-10, Issue #6)

- **결과(1-run, branch `framework-java/6-mesh-ingress`, 커밋 6ce6a0d90e, push 안 함)**: request-serial 1024 ratio 0.0719 → **0.2887**, 4096 0.0769 → 0.2802,
  send-saturation 1024 0.0177 → 0.0251, 4096 0.0265 → 0.0426. request-window Framework 2,266 → 5,367 ops/s. 평균 지연 2.2 ms → 0.57 ms.
  send drain 2,477 ms → 299 ms. 6개 셀 모두 errors 0·submitted=completed=received. Java 전체 test·contractTest 통과.
- **제거한 것(규칙 7 → 3)**: 1 ms park 폴링 → `waitForReadable` + 64건/4 MiB/2 ms batch, receive 전 permit 확보,
  service mailbox와 1건 claim drain 제거, topology/liveness의 요청별 executor 왕복 → 변경 시점 precompute한 WRR plan,
  channel serial queue가 handler executor를 직접 소유(동기 완료 handler의 추가 hop 제거), `Inbound` 배열 clone 2회 제거.
- **남은 병목(감독자 검증)**: `ZLinkChannelEnvelope.encodeHeader`가 message마다 Jackson `ObjectNode`를 만들고 `writeValueAsBytes`로
  직렬화한다(`zlink-framework-core/.../messaging/ZLinkChannelEnvelope.java:155-185`). 수신 쪽은 같은 header를 JSON parse한다.
  반면 raw 드라이버는 **같은 wire를 상수 byte 배열로 미리 만들어 둔다**(`framework/bench/grpc/java/shared/.../RawWire.java:21-38`).
  즉 두 경로의 wire는 같고, 차이는 "요청마다 header를 다시 만드는 비용"이다. wire 계약을 바꾸지 않고도
  (a) 채널·메시지·kind·contentType별 상수 prefix를 미리 인코딩해 재사용, (b) tree model 대신 streaming generator로 재사용 버퍼에 기록,
  (c) 수신 쪽 streaming parse로 줄일 수 있다. **codec을 우회하거나 벤치 전용 경로를 만들지 않는다.**
- **결정(감독자)**: P3 범위 = envelope header 인코딩/디코딩 비용 제거. 네 언어 공통 문제인지 먼저 확인하고 언어별로 같은 형태로 고친다.
  sol의 "typed JSON serializer는 공개 계약이라 손댈 수 없다"는 판단은 **부분 수용** — wire 형식은 그대로 두되 인코딩 방법은 구현 재량이다.

## FB-060 — gRPC 벤치의 raw 드라이버가 언어마다 다른 per-message 비용을 진다 (2026-09-10, Issue #25)

- **계기**: bindings perf의 relay 과설계(빈 `Message` 할당 + `move` + `close`)를 고친 뒤(커밋 a7ffc3cf58·e7d97a9638·160a1fbefa,
  기록 6fa350a0c2) 사용자가 같은 실수가 벤치에도 있는지 검토를 지시했다.
- **판정: 같은 형태는 벤치에 없다.** 벤치의 raw 서버는 c/zlink와 wire 바이트를 맞추려고 응답을 새로 인코딩하므로 relay가 아니다.
  cpp의 prvalue `std::move`만 관용형이 어긋난 채 남아 있다(`bench_zlink_cpp_server.cpp:67,69`, `bench_cpp_client.cpp:576,602,626`).
- **대신 확인된 비대칭(감독자 재검증 완료)**: (1) .NET raw가 payload를 메시지마다 두 번 해석한다(`ZLinkRawServer/Program.cs:62,147-152,156`),
  (2) Java raw가 응답마다 payload 크기 복사를 3회 더 한다(`ZLinkRawBenchServer.java:94-97,104`), (3) Java raw가 응답 경로에서
  메시지마다 `System.getenv`를 호출한다(`:106`), (4) Node raw가 메시지마다 `Received.close()`를 응답 임계경로에서 부른다
  (`node/zlink-raw-server/main.js:91`), (5) 클라이언트 payload 인코딩 복사가 언어별로 1~4회로 다르다.
- **영향**: (1)~(4)는 모두 분모인 raw를 느리게 만든다. 따라서 **현재 보고된 framework/raw 비율은 낙관적이며 실제 격차는 더 크다.**
  2차 캠페인 비교표(published)도 이 드라이버로 측정했다.
- **결정(감독자)**: Issue #25로 등록. raw 드라이버의 per-message 작업을 (a) 수신 payload 1회 스캔, (b) 응답 payload 1회 복사,
  (c) 계측 스위치는 시작 시 1회 조회로 통일하고 대조표를 벤치 문서에 남긴다. 수정 뒤 3-run 재측정으로 비교표를 갱신한다.
  P2 판정(0.90)은 이 수정 뒤의 raw 기준으로 한다.
- **추가 확인(framework 쪽, 2026-09-10)**: framework **서버**는 네 언어 모두 깨끗하다(계측이 디코딩된 객체를 읽고,
  per-message env·로그가 없다). framework **클라이언트**에는 반대 방향의 하네스 비용이 있다 — C++가 완료 대기를
  `await_ready` + `std::this_thread::yield()` spin으로 하고(`cpp/client/bench_cpp_client.cpp:795,821`; raw는 `_poller.wait` `:486`),
  완료마다 vector 중간을 `erase`하며(`:817`, window 100이면 완료 1건당 최대 100회 이동; raw는 `remove_if` 한 번 `:471-474`),
  Java·Kotlin이 payload를 한 번 더 복사한다(`FrameworkStack.java:88-90`). Node는 framework 클라이언트가 없어 행이 UNSUPPORTED다.
  **즉 raw 쪽 결함은 비율을 높이고 framework 클라이언트 결함은 비율을 낮춘다 — 두 방향이 섞여 현재 숫자는 어느 쪽으로도
  신뢰할 수 없다.** Issue #25의 완료 조건에 framework 클라이언트의 event 기반 대기·O(1) 완료 정리·복사 없는 payload를 더했다.

## FB-061 — .NET framework P2 1차: 채택 불가(send-saturation 4096 회귀, persistent worker 미완) (2026-09-10, Issue #5)

- **결과(1-run, branch `framework-dotnet/5-dispatch-batch`, WIP 커밋 62d1cff790 — 채택 아님)**: 비율 0.0023~0.2762로 전 셀 0.90 미달.
  request-serial 1024 0.2064 → 0.2159, request-window 1024 0.1422 → 0.1532로 소폭 개선. **send-saturation 4096은
  0.0116 → 0.0023(before 대비 0.1848배)로 회귀**했고 target backlog가 5 GiB급으로 쌓였다. 이 회귀 때문에 커밋을 채택하지 않았다.
- **구현한 것**: 회전당 최대 64건 drain, binding receive 전 permit 예약, framework multipart를 직접 할당한 native `Message`에 기록,
  pump가 한 claim에서 64건까지 받고 batch callback으로 넘김, node route batch당 detached task 1개(요청별 task·supervisor 등록 제거).
- **미완**: 스펙 08 W1~W5의 **persistent application worker**. batch 사이에 살아 있는 worker 대신 batch마다 task를 만든다.
- **검증**: Unit 2,034/2,034, Contract 77/77 통과. 첫 실행에서 raw ingress가 읽을 frame이 없는데 다음 permit waiter를 미리 예약해
  multicast child가 굶는 race가 1회 재현됐고, poll 결과가 있는 turn만 waiter를 만들도록 고쳐 20회 반복과 전체 suite가 통과했다.
- **binding 쪽 관찰**: published binding 0.17.6 단독 repro에서 completion reservation 65,536개를 채우면 tokenless request submit이
  `Backpressured`(errno 11)로 끝난다. binding·Core 계약의 정상 terminal이므로 Framework mapper를 바꾸지 않았다(Issue #19와 별개).
- **결정(감독자)**: 2차 범위 = (1) persistent application worker, (2) **send-saturation 4096 회귀와 target backlog 원인 규명 우선**
  — 프레임워크 target이 4096에서 소비하지 못하는 이유를 계측으로 특정한다(raw는 361k msg/s인데 framework는 814 msg/s),
  (3) 스펙 08 §3~§4 순서 준수. 수치를 맞추려고 벤치 조건·HWM·timeout을 바꾸지 않는다.

## FB-062 — Java 수신 pump는 platform thread여야 한다. 가상 thread면 send 지연이 125 ms (2026-09-10, Issue #75, PR #80)

- 같은 커밋을 pump 종류만 바꿔 잰 1-run(1024 B, `.artifacts/vt-compare/`):

  | 패턴 | 가상 thread | platform thread | 차이 |
  |---|---:|---:|---:|
  | send-saturation 처리량 | 14,356 msg/s | 67,712 msg/s | **4.7배** |
  | send-saturation 지연 | 125.462 ms | 0.247 ms | **1/508** |
  | request-serial 처리량 | 1,927.8 ops/s | 2,233.2 ops/s | +15.8% |

- 이 pump는 socket 하나를 blocking으로 기다리는 전용 실행 단위다. 가상 thread의 이점(대기 중 carrier 반납)이
  없고 mount/unmount 비용만 남는다.
- **경위 정정**: 처음에는 "가상 thread에서 멀티파트 수신이 실패한다"는 보고로 임시 우회를 시작했다.
  감독자가 그 재현을 현재 main에서 다시 돌리니 **네 조합(virtual/platform × forceYield on/off) 모두 정상**이었다.
  정확성 문제는 재현되지 않는다. 성능 차이만 실재하므로 성능 수정으로 채택했다.
- **남은 질문**: 가상 thread에서 blocking 수신이 왜 이렇게 느린지, 멀티파트 실패 보고가 무엇이었는지(Issue #75 유지).
  다른 언어에도 같은 형태가 있는지 확인이 필요하다.

## FB-063 — Java 제출 경로의 읽기 전용 조회 3회가 177 µs를 쓴다. 본문은 0.57 µs (2026-09-10, Issue #78)

- 프로파일(`.artifacts/codex/java-client-profile/`):

  | 구간 | framework | raw binding |
  |---|---:|---:|
  | 제출 진입 → binding submit | **193.20 µs** | 1.35 µs |
  | 요청당 Java 스레드 전달 | 12회 | 3회 |

  193 µs 중 **177.55 µs가 registry 조회 3회의 lane 왕복**, 조회 본문 합계는 **0.57 µs**다. 300배다.
- 문제 코드: `ZLinkChannelRuntime.java:1097,1106`이 제출 전에 `hasClientRegistration`·`spotRouterNode`를 부르고,
  둘 다 맵 조회 하나를 `inStateLane`으로 감싼다(`ZLinkChannelSocketRegistry.java:642-648`, `:1368-1370`).
  `sendToChannel`도 같은 경로다 — **채널 API 전체가 이 비용을 낸다.**
- **이 발견이 앞선 라운드의 결과를 설명한다.** 감사로 뽑은 클라이언트 항목 7개(요청별 timer 2개, UUID 2개,
  이중 등록, 이중 진입, 여분 continuation, 복사)를 모두 없앴는데 +7.4%였다(PR #74). 그 항목들은 0.57 µs 쪽에
  속한 비용이었다. **문제는 무엇을 하느냐가 아니라 그 일을 어디서 하느냐였다.**
- 같은 처방이 이미 통했다: Issue #68(PR #76)에서 handler scope 조회의 스레드 왕복을 없애 send 지연을
  120.4 ms → 0.301 ms로 줄였다.
- 스펙 08 E2는 선택이 "선택 소유자의 turn 하나 안에서 상수 시간"이어야 한다고 정한다. 지금은 turn을 세 번 왕복한다.

## FB-064 — gRPC 벤치의 비교 짝이 어긋나 있었다 (2026-09-10, 사용자 지적, Issue #13 재정의)

- 세 스택이 서로 다른 토폴로지·주소 지정을 쓴다: gRPC는 특정 대상과 1:1, zlink core(raw)는 **DEALER→ROUTER**,
  zlink framework는 **RouteMesh `sendToChannel`**(ROUTER↔ROUTER 위 채널 단위 라운드로빈)이다.
  그래서 비율이 "framework 계층의 비용"이 아니라 "토폴로지 차이 + framework 비용"을 섞어 보고한다.
- **결정(사용자)**: gRPC 벤치는 gRPC와 비교하는 것이 목적이므로 셋을 맞춘다 — core는 **ROUTER↔ROUTER**,
  framework는 **`sendToNode`/`requestToNode`**(RID 직접). 패턴은 `request-serial`·`request-backpressure`·
  `send-saturation` 셋으로 줄인다. `request-window`는 깊이를 밖에서 강제해 도달 깊이를 보고하므로 뺀다.
  서버는 1개 유지.
- 모델 사이 비교(DEALER→ROUTER, `ToChannel`, ClientServer)는 **별도 벤치**로 분리한다. 특히 `ToChannel`과
  `ToNode`를 나란히 재면 채널 선택 비용이 그 차이로 드러난다 — 가치는 있으나 gRPC 비교표에 섞을 값이 아니다.
- 재측정 뒤 이전 숫자와 직접 비교하지 않는다. 분모가 달라진다.

## FB-065 — raw ROUTER↔ROUTER request-backpressure 붕괴는 벤치 raw 클라이언트가 bindings perf 구조를 따르지 않은 것; send_ready 콜백 부활은 하지 않는다 (2026-09-10, 사용자 결정)

- 증상: Java raw ROUTER↔ROUTER `request-backpressure` 0.2~0.4/s(warmup 포기 4만 건대, 3-run 전부, 2026-09-09
  `s2r_run{1,2,3}`부터 동일), 이전 `request-window`(100) raw 0(Issue #12/FB-050). 서버는 받은 요청을 전부 응답, 오류 0,
  CPU 0.7%. 같은 binding으로 framework ToNode 5,457/s, request-serial raw 6,153/s는 정상.
- 확인한 것: 벤치 raw 클라이언트(`RawStack`, `BenchDrivers.runBackpressure`)는 poller 없이 tight loop로 제출만 하고
  완료는 binding runtime pump에 맡긴다. bindings perf의 같은 구성 `PerfMultiSocketReqRep`(ROUTER↔ROUTER request,
  상한 없음)은 client socket을 public poller에 `POLLCOMPLETION`으로 등록해 자기 poll 루프에서 완료를 drain하고 turn마다
  socket당 요청 하나를 제출한다. Core는 HWM에 막힌 submit마다 대기 토큰만 보관하고 payload는 호출자가 든다(0.17 계약 B,
  D-B79/D-B85). 감독자가 처음 제시한 "Core가 대기 전부를 깨우고 binding이 전부 재시도한다"는 설명은 4개 binding 코드 조사
  결과(token-matched 재시도만 수행) 뒷받침되지 않아 철회했다. 벤치 raw 행에 application 상한을 두자는 제안도 결함을 가리는
  것이라 철회했다(사용자).
- 결정(사용자): 벤치 raw 클라이언트를 perf 구현(best practice)대로 다시 쓴다. Issue #12를 그 내용으로 다시 썼고 job
  `bench-java-raw-perfshape`가 수행한다. 통과 기준은 raw request-backpressure 3-run warmup 포기 0·오류 0, 다른 패턴 회귀 없음.
  다른 언어 raw 드라이버도 각 언어 perf 샘플 대비 확인해 같은 Issue 아래 후속.
- send_ready 콜백 부활 검토(사용자 질문): 하지 않는다. 0.13에서 hint 콜백은 "재시도해볼 만하다"는 뜻뿐이라 admission 정책을
  세울 수 없고, 콜백 안 재개는 콜백 내 submit 금지·send-sequence gate로 EINVAL 69~88%·Core 스레드 블로킹을 요구해
  폐기됐다(`doc/plan/archive/core-send-completion-design.ko.md:29-32`, 커밋 2cea03c016). 0.16의 Core 소유 pending pool은
  무제한 내부 큐가 되어 HWM이 흐름 제어를 잃어 폐기됐다(D-B71~D-B79, D-B85). 관리형 런타임은 콜백을 받아도 자기 루프로
  넘겨야 하므로 큐+wake가 필요하고, 그것이 현재의 POLLCOMPLETION pull이다. 남는 완화는 binding public API에 perf 샘플의
  completion 루프 형태를 유틸로 제공하는 것(1.0 뒤 후보).

### FB-065 추가 (2026-09-10 저녁) — 깊이 사다리와 사용자 판정: 벤치가 backpressure를 보지 않은 것이 잘못, binding 결함 아님

- perf 구조 1차 결과: raw 6셀 오류 0으로 완료했으나 socket 1개라 `peak_in_flight=1`, request-serial과 같은 8k/s.
- 깊이 사다리(`.artifacts/codex/java-raw-depth-ladder/K-*`, 1024B, socket 1개, turn당 K건 제출):

  | K | 처리량/s | p95 | peak_in_flight | 오류 |
  |---:|---:|---:|---:|---:|
  | 1 | 8,568 | 0.17 ms | 1 | 0 |
  | 10 | 69,714 | 0.21 ms | 30 | 0 |
  | 100 | 361,520 | 0.58 ms | 699 | 0 |
  | 1,000 | 4,688 | 30.5 s | 26,288 | 6,558 timeout |
  | 10,000 / 100,000 | 128 / 102 | 38 s / 34 s | 24k / 30k | 포기·timeout 수만 건 |

  K=10은 bindings perf multi(socket 100개, 69,272/s)와 같다. HWM 아래 깊이에서는 socket 하나로 361k/s(gRPC 209k~258k보다 높다).
  send HWM을 넘겨 대기 토큰을 쌓으면 요청이 timeout(30 s)으로 실패한다. HWM은 accounted **byte** 기준(manual 기본 4,096,000 bytes,
  auto HWM은 context budget의 water-filling)이며 메시지 건수로 환산하지 않는다(사용자 지적, 감독자의 "1 MiB ≈ 1,000건"은 오류).
- 감독자는 HWM 초과 구간의 붕괴를 binding 결함 후보로 올렸으나 **사용자 판정: binding에는 문제가 없고 벤치가 잘못 작성됐다.**
  올바른 클라이언트는 admission backpressure(POLLOUT 거짓)에서 제출을 멈추고 재개 신호에서 이어간다(규격 §2,
  `PERF_MULTI_TEST_POLICY.md` §1.1; C reference `perf_multi_socket_reqrep.hpp:832-870`). HWM을 넘겨 토큰을 수만 건 쌓는 것은
  지원하는 사용 방식이 아니다. 2차 job `bench-java-raw-pollout`: public poller `POLLOUT|POLLCOMPLETION`, POLLOUT이 참인 동안 제출.
- 기준값(감독자 직접 실행, `bindings/java/perf/multi/run_benchmarks.sh --pattern ROUTER_ROUTER_REQREP --transports tcp --msg-sizes 1024
  --duration 5`, Core 0.17.5, JDK 25): clients=100 **248,159/s**(p95 0.450 ms), clients=1 **8,542/s**(p95 0.081 ms). 벤치 raw
  1 socket(7.7~8.8k/s)과 clients=1이 같다 — binding에 문제 없음 확인. **사용자 결정: gRPC 비교는 socket 1개로 한다.** 2차 job
  `bench-java-raw-pollout`(POLLOUT 게이트) 착수. 완료 구간 프로파일(`.artifacts/codex/java-completion-profile/`): 완료 구간 차이는
  15 µs뿐이고 `requestToNode` 제출 구간이 194 µs(왕복 3회) → Issue #85, job `java-tonode-lane`.

## FB-066 — #85 채택. registry turn 3→1은 맞으나 그 개선을 고정하는 테스트가 없다 (2026-09-10, 감독 검증, PR #107, Issue #108)

job 보고를 감독이 직접 재검증했다.

- **채택.** 감독이 이 기계에서 직접 실행: `zlink-framework-core:test` 1,374 + contractTest 27 + kotlin
  contractTest 17 + provider-abstractions 4 + spring-boot-starter 48 + testkit 48 = **1,518 테스트, 실패 0**
  (skip 1). Core 0.18.0 release prefix, binding은 `ZLINK_JAVA_BINDINGS_SOURCE` includeBuild.
- **flow context는 제거된 것이 아니라 이동했다.** diff만 보면 `ZLinkFlowContext.current()/enter/suppress`
  래핑이 사라진 것처럼 보이나, `submitInStateLane` 하나로 옮겨져 세 제출 경로에 균일하게 적용된다.
  spec 26/27의 `flow`·`corr` 상관은 유지된다. 규칙이 3곳 중복 → 1곳 소유로 줄었다.
- **원래 블로커는 오진이었다.** "공유 Maven에 binding 0.18.0이 없어 막힘"으로 기록돼 있었으나, 실제
  원인은 worktree가 main보다 447파일 뒤처져 pin이 **0.17.7**(존재하지 않는 버전)이었던 것이다.
  main 병합으로 풀렸다.
- **남은 것**: 새 `ZLinkNodeSubmitTurnTest`(586줄)는 라우팅·timeout·metadata·오류 경로를 검증하지만
  **lane turn 횟수를 assert 하지 않는다** — 3 turn으로 되돌아가도 통과한다. Issue #108로 분리했다.

## FB-067 — Node의 1 ms ingress 타이머는 spec gap이 아니다. 메커니즘은 이미 있고 공개돼 있지 않을 뿐이다 (2026-09-10, 감독 재검증, Issue #50 → #111)

Issue #50의 job(astra)이 "Node binding에 비동기 ordinary receive readiness 공개 계약이 없다"며
**D(계약 부재)** 로 보고하고 코드 변경 없이 종료했다. 감독이 인용 코드를 직접 열어 **기각**했다.

- `bindings/node/native/src/addon_core.cc:2114-2196` `socket_readable_watch_start(socket, callback)`이
  `ZLINK_OPT_FD`로 fd를 얻어 `uv_poll_init_socket` + `uv_poll_start(UV_READABLE)`를 건다. 폴링도
  타이머도 없는 진짜 libuv readiness다.
- `addon_exports.cc:80-81`이 `socketReadableWatchStart`/`Stop`으로 내보낸다.
- **이미 제품 경로에서 쓰인다**: `completion_owner.ts:662-668` `ensureRuntimeWatch()`가 completion
  소켓에 걸고 `runtimeWake(status)`에서 drain 한다. async resource 이름도 `"zlink:completion"`이다.
- 막힌 것은 `bindings/node/src/index.ts` 공개 export에 없다는 것뿐이다. framework는 공개
  `@zlink-systems/zlink`만 쓰므로 닿을 수 없고, 그래서 `node-raw-mesh-backend.ts:1504-1515`가
  `setTimeout`으로 깨어난다.

따라서 분류는 D가 아니라 **B(기존 결함) + 공개 API 추가 하나**다. AGENTS.md §3에 따라 공개 API
추가는 설계 변경으로 분리해 사용자에게 보고한다 → **Issue #111**, 사용자 결정 대기.

검토한 대안: ① framework가 `setImmediate` spin(raw 벤치 방식) — 스펙 08 §4 I0·§7(a) busy polling
금지 위반, 코어 하나 소모. ② `Poller`에 fd를 넣고 `wait` — 동기 차단이라 Node 이벤트 루프를 막는다.
둘 다 기각.

**Node는 이것 없이 0.90에 도달할 수 없다.** 왕복마다 1 ms가 고정으로 붙는다.

## FB-068 — #48 진단 승인. dispatch별 DI scope 제거는 공개 계약 위반이므로 제외한다 (2026-09-10, 감독 승인, Issue #48)

job이 AGENTS.md §3의 2단계 규칙대로 1단계 진단만 내고 승인을 기다렸다. 감독이 인용 근거를 직접
확인하고 **승인**했다.

- `git cherry origin/main framework-dotnet/5-dispatch-batch` → 11개 커밋 전부 `-`(patch-equivalent).
  그 브랜치를 merge·cherry-pick 할 것이 없다. 감독이 직접 실행해 확인했다.
- `Runtime/Execution/ZLinkStateLane.cs:208`의 `Interlocked.Exchange(ref _scheduled, 0)` 존재 확인 —
  FB-061 lost-wakeup 수정은 유지한다.
- **항목 7(b) 제외**: 인용된 공개 계약
  `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/03-configuration-topology.ko.md` §4가
  "Node direct·Channel send/request와 classic fanout 구독 handler를 **실행할 때마다 DI scope를 하나
  만든다**"를 명시한다. 감독이 직접 열어 확인했다. scope 자체 제거는 **D**이므로 구현 금지.
  허용되는 것은 7(a) — activation 경로를 등록 시점에 compile/cache 하고 scope 의미는 유지.
- 구현 승인 범위: 항목 1·2·3·4·6·7(a)·8·9 (전부 B). 항목 5는 이미 해결됨.

진단 표는 `.artifacts/codex/dotnet-lane-48/diagnosis.md`에 보존했다(job.log에만 있던 것을 꺼냈다).

## FB-069 — 환경 결함 셋을 고쳤다: local-package 경로 별칭, job.sh xhigh, Rust 테스트의 sleep 의존 (2026-09-10, 감독)

캠페인 진행을 실제로 막던 것들이다.

- **local-package가 두 번째 실행부터 항상 실패했다**(Issue #112, PR #113). `package-cache.py:267-269`가
  `<staging>/build`를 영속 build tree로 가는 심볼릭 링크로 만드는데 staging 이름에 pid가 들어간다.
  C·C++ 스크립트가 그 별칭을 CMake에 넘겨 `CMAKE_CACHEFILE_DIR`에 박혔고, 다음 실행은 경로 불일치로
  반드시 죽었다. `readlink -f`로 실제 경로를 넘기게 고쳤다 — build tree 하나에 이름 하나. 보정 분기는
  넣지 않았다. 수정 뒤 8개 언어 전부 패키징 성공.
- **Rust 바인딩 테스트가 부하에서 깨진다**(Issue #110). `ownership_tests.rs:217`과
  `contract_tests.rs:388`이 `NotConnected`(errno 107)로 실패했으나 단독 실행은 각각 9/9·26/26 통과.
  원인은 inproc 연결 완료를 `thread::sleep(50ms)`로 추정하는 것(CONTRIBUTING §5 금지). 이 flake로
  로컬 패키징이 세 번 막혔다. **Core 0.18.0 회귀가 아니다.**
- **job.sh가 `--effort xhigh`를 거절했다**(Issue #115, PR #116). 사용자 결정(2026-09-10)은 astra를
  쓸 때 항상 xhigh다.

## FB-070 — PR 검증 워크플로우를 냈다. framework C++은 부트스트랩 때문에 분리한다 (2026-09-10, Issue #16, PR #118 / #117)

`build.yml`은 dispatch 전용이라 PR을 받지 않고, framework .NET·Node만 PR CI가 있었다. Core·binding·
framework Java에는 **PR 검증이 전혀 없었다** — 네 언어의 framework 런타임을 동시에 고치는 중에
회귀가 그대로 들어온다.

`pr-verify.yml`을 냈다. GitHub의 paths 필터가 workflow 단위인 문제는 union 필터 + `changes` job의
`git diff --name-only`로 풀었다(외부 action 없음). core job은 CONTRIBUTING §1·§6의 명령 그대로
(ctest 전체, single_lane ×2, header mirror 대조, `git diff --check`, C++·Python 스모크). framework-java
job은 체크아웃 Core를 소스 빌드하고 binding을 `ZLINK_JAVA_BINDINGS_SOURCE`로 includeBuild 하므로
릴리스 자산이나 공개 Maven에 의존하지 않는다 — **VERSION을 올리는 PR도 검증된다.**

framework C++은 ① apt 의존성 8종 ② hiredis·redis-plus-plus 소스 빌드(Debian `libhiredis-dev`에
`hiredis-config.cmake`가 없다) ③ `find_package(zlink_cpp ... CONFIG REQUIRED)`용 binding package가
선행이라 같은 PR에 넣으면 경량 워크플로우가 아니게 된다. Issue #117로 분리했다.

## FB-071 — submit 결과 객체 캠페인 G6 perf 판정: criterion 2·4 PASS, 회귀 없음. ccu=1>ccu=100 역전은 harness 단일스레드 특성(C control·3언어·Core 버전·과거 결과로 4중 확증) (2026-09-11, 머신 B 감독)

바인딩 submit 결과 객체 캠페인(#88~#96, #90 머지 완료)의 G6 perf 재측정. Core는 0.18.0 released prefix로 고정(`~/.cache/zlink/core/0.18.0/linux-x64`), `ROUTER_ROUTER_REQREP` tcp/1024, runs=3.

**criterion 2 (ccu=1 깊이 1 탈출) — PASS.** cpp ccu=1: 옛 루프 **7,933/s**(깊이 1, 0.17.5에서도 재현·문서 "8.5k"와 일치) → 새 §5 루프 **300,108/s** (약 38×). C reference(275k)와 동급. §5가 "`OK`면 즉시 연속 제출, `BACKPRESSURED`만 `admitted` 대기"로 단일 소켓을 HWM 깊이까지 파이프라인.

**criterion 4 (회귀 없음) — PASS.** same-Core(0.18.0) 전후 cpp ccu=100 tcp/1024: 옛 루프 150,518 → 새 §5 156,979 (+4.3%). 과거 저장 결과와 cpp-to-cpp 교차(clients=100):
- tcp/1024: 과거 0.17.5(커밋 6edf7b95) 97,403 → 0.18.0 현재 150~157k (상승)
- tcp/64: 과거 0.17.5 137,755 → 0.18.0 현재 ~142,323 (동급)
Core 0.17.5→0.18.0 전환도 회귀 아님(C RR/1024/c100 185k→223k, cpp 97k→157k 상승).

**ccu=1 > ccu=100 역전 조사(사용자 제기).** cpp 새 §5: ccu=1=300k > ccu=100=157k. "ccu=100이 더 높아야 정상" 직관과 반대라 §5 버그·Core 회귀를 의심해 4중 검증:
1. **C reference(이 캠페인 미변경) control**: ccu=1=275k > ccu=100=223k — 미변경 코드도 동일 역전 → §5 버그 아님.
2. **3언어(C·C++·go)**: 전부 ccu=1 > ccu=100 (go 125k>92k) → 언어 공통 = harness 특성.
3. **Core 버전(0.17.5 vs 0.18.0)**: ccu=100 하락 없음(상승) → Core 회귀 아님.
4. **과거 저장 결과**: size축 확인(논의는 1024B, 사용자가 64B로 오인). 전 size·버전에서 c100 유지·개선.
원인: 벤치 harness가 **단일 협조 런타임 스레드**로 모든 클라 구동(PERF §1.3). 옛 루프는 ccu=1이 depth-1(7.9k)로 스레드를 굶겨 ccu=100이 필요했으나, §5가 ccu=1을 300k로 고쳐 **단일 클라가 이미 스레드 천장 포화** → ccu=100은 100코루틴 코디네이션 오버헤드만 추가. **ccu=100 자체는 저하 없음.** 벤치 버그 아님 — harness는 의도적 단일 런타임(멀티코어 스케일이 아니라 binding async 효율 측정).

**남은 관찰(§5 무관·선재)**: cpp ccu=100(157k)이 C ccu=100(223k)보다 낮음(c100/c1: cpp 52% vs C 81%) — 바인딩 async 런타임 per-client 오버헤드가 C reference보다 큼. baseline(150k)부터 있던 선재 특성, 별도 최적화 영역.

**criterion 3 (gRPC Java raw 3-run)**: raw 드라이버 코드(RawStack·BenchDrivers.runRaw)는 #90(#141)으로 머지·assemble 검증됨. 전체 gRPC 3-stack 비교 실행은 `systems.zlink:zlink:0.18.0` 공유 Maven 로컬 패키지(handoff §5 전제) 발행이 선행 — 후속으로 처리. 바인딩 캠페인 판정의 blocker 아님.
