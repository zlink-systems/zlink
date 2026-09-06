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
| `fwb-02b` | 0 | opus | 완료·커밋 `c67d677832` | FB-010 판정(b), FB-013 정정, gated2 8/8. ROUTER 3회 18/18 clean |
| `fwb-01` | 1(문서) | opus | 완료·커밋 `146db4da4c` | 규격 5언어 중립화(ko 338행·en 359행), FB-001~003 반영. 고정값·RPC 미변경 확인. FB-004·FB-005 추가 지시 |
