# .NET framework perf runner — phase 1 결과

감독 검토용 작업 결과다. Phase 1 harness와 Session/Channel 실행 파일을 구현하고, 일반 30초 기준 셀 3개를 확보했다. **전체 6개 셀의 성공 완료는 아니다.** RouteMesh 4096은 일반 실행의 warmup 실패로 measured 구간을 시작하지 않았으며, ClientServer 2개 셀은 공개 readiness 계약 불일치로 중단했다. Framework runtime 또는 계약 문서는 변경하지 않았고 commit하지 않았다.

## 구현 파일과 실행 경계

아래 경로는 `framework/languages/dotnet/perf/` 기준이다. `Zlink.Framework.sln`에 Shared, ServerSupport, Client, SessionServer, ChannelServer, Tests 6개 프로젝트를 perf solution folder로 등록했다. 서버는 기존 bench와 같은 public Framework ProjectReference를 사용하며 standalone CS client는 public Stream Connector를 사용한다.

| 파일/프로젝트 | 구현 내용 |
|---|---|
| `Directory.Build.props`, 각 `*.csproj`, `.gitignore` | net8.0 Release, 기존 parent build 설정 및 public 프로젝트 참조, perf 소스의 warning을 오류로 처리 |
| `ZLink.Framework.Perf.Shared/Contracts.cs`, `Payload.cs` | §12/§15.2 typed DTO, schema v2, decimal-string 64-bit 필드, 전체 Base64/byte/identity 검증, Stopwatch ns 변환 및 process clock 근거 |
| `ZLink.Framework.Perf.Shared/Measurement.cs`, `ProcessSampler.cs`, `MetricCatalog.cs` | warmup/drain/reset/measurement/settle, cohort 보존, public/harness/language 오류 구분, 자원 sampler, 적용되지 않는 metric의 null+reason |
| `ZLink.Framework.Perf.Shared/Histogram.cs`, `histogram-bounds.json` | 공통 bucket 원본 하나, U64 count, arbitrary-precision sum, 정수 nearest rank, overflow 및 sample 부재 사유 |
| `ZLink.Framework.Perf.ServerSupport/ServerApplication.cs`, `PublicMetricCollector.cs` | application admin/trigger 별도 HTTP listener, 공개 status/ResetCapacityMetrics, 표준 MeterListener의 provider 이름·단위·label 보존 |
| `ZLink.Framework.Perf.ServerSupport/MessageFlowFileListener.cs` | 별도 진단 실행에서 기존 Framework ActivitySource의 message-flow를 파일에 보존 |
| `ZLink.Framework.Perf.Client/Program.cs`, `MetricsClient.cs`, `SessionEchoOnlyScenario.cs` | CS connector 분할·준비·echo, stdin/stdout phase 제어, application HTTP trigger; 측정 public call이 scenario 본문에 직접 보임 |
| `ZLink.Framework.Perf.SessionServer/Program.cs`, `PerfSession.cs` | STREAM Session echo 전용 서버; Actor/Spot/Store 등록 없음 |
| `ZLink.Framework.Perf.ChannelServer/Program.cs`, `ChannelEchoOnlyScenario.cs` | 별도 source/target PID, manual RouteMesh 또는 ClientServer, public RequestToChannel 및 typed reply handler |
| `scripts/run_perf.sh`, `run_single.sh`, `run_diagnostic.sh`, `runner.py` | CLI consumer preflight, matrix/단일 셀/진단, config와 endpoint manifest, verified port 예약, 소유 process cleanup |
| `scripts/results.py` | owner별 원본 집계, result/summary/index 파일, failed/invalid/unsupported 구분; 실패를 성공 baseline으로 채택하지 않음 |
| `scripts/dotnet-env.sh`, `collect_env.sh`, `environment.py`, `redis-common.sh` | 지정된 .NET/NuGet 환경, artifact·OS 수집, sample Redis template 재사용 |
| `ZLink.Framework.Perf.Tests/HarnessContractTests.cs`, `scripts/test_harness.py` | DTO, histogram, phase/reset, CLI consumer, owner 집계, 실패 원본 검증 |

Phase 1 밖의 Actor/Spot/Worker/PubSub 실행 파일은 생성하지 않았다. 해당 common DTO와 null metric은 준비했으며, 미구현 scenario/option은 preflight에서 거부한다. Script는 process·파일·barrier만 소유하고, public call과 monotonic 측정은 C# owner에 둔다. 서버마다 admin 코드를 복사하는 방식과 공통 ServerSupport를 비교해, 같은 endpoint/reset 규칙을 한 곳에 두는 구성을 택했다. Framework runtime 규칙 수: 변경 전=변경 후(변경 0건).

## 실행 명령

아래 명령은 저장된 실행을 재현하는 정확한 입력이다. 결과 경로 덮어쓰기를 거부하므로 재실행할 때는 `--run-id`와 `--output`을 새 값으로 함께 바꾼다. 각 shell runner가 환경 설정과 `/tmp/zlink-samples-gate.lock`의 `flock --exclusive --close`를 적용한다. CLI 기본 부하는 10,000이지만 이번 검증은 처음부터 CS 64 connectors / Channel 16 logical streams, inflight 1로 고정했다. 10,000 부하의 처리량 또는 연결 준비 성공을 검증한 결과는 아니다.

```bash
cd /home/hep7/project/zlink
source framework/languages/dotnet/perf/scripts/dotnet-env.sh
# TMPDIR=/dev/shm/zlink-tmp-dotnet
# ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib
# UseSharedCompilation=false MSBUILDDISABLENODEREUSE=1 DOTNET_CLI_TELEMETRY_OPTOUT=1
# NUGET_PACKAGES=/dev/shm/zlink-tmp-dotnet/nuget-${pkg_hash:0:16}
# pkg_hash: .artifacts/wsl/nuget/Systems.Zlink.0.17.0.nupkg SHA-256

bash framework/languages/dotnet/perf/scripts/run_perf.sh \
  --connections 64 --logical-streams 16 \
  --duration-seconds 2 --warmup-seconds 1 \
  --run-id phase1-smoke \
  --output /dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke

bash framework/languages/dotnet/perf/scripts/run_perf.sh \
  --scenario session-echo-only --connections 64 \
  --duration-seconds 30 --warmup-seconds 5 \
  --run-id phase1-full-session \
  --output /dev/shm/zlink-perf-dotnet/perf-results/phase1-full-session

bash framework/languages/dotnet/perf/scripts/run_single.sh \
  --scenario channel-echo-only --channel-topology routemesh \
  --logical-streams 16 --payload-size 1024 \
  --duration-seconds 30 --warmup-seconds 5 \
  --run-id phase1-full-routemesh-1024 \
  --output /dev/shm/zlink-perf-dotnet/perf-results/phase1-full-routemesh-1024

bash framework/languages/dotnet/perf/scripts/run_single.sh \
  --scenario channel-echo-only --channel-topology routemesh \
  --logical-streams 16 --payload-size 4096 \
  --duration-seconds 30 --warmup-seconds 5 \
  --run-id phase1-full-routemesh-4096 \
  --output /dev/shm/zlink-perf-dotnet/perf-results/phase1-full-routemesh-4096
```

실패한 셀을 포함한 matrix는 각 셀 결과를 보존한 뒤 exit 1을 반환한다. 이후 다른 셀로 진행하는 것은 실패 operation 재시도가 아니다. ClientServer는 smoke에서 필수 공개 상태를 확인하지 못했으므로 별도 30초 실행을 시작하지 않았다.

```bash
# 기존 flow를 켜는 별도 진단. 진단 결과는 baselineEligible=false다.
bash framework/languages/dotnet/perf/scripts/run_diagnostic.sh \
  --scenario channel-echo-only --channel-topology routemesh \
  --logical-streams 16 --payload-size 4096 \
  --duration-seconds 2 --warmup-seconds 1 \
  --run-id phase1-diag-routemesh-4096 \
  --output /dev/shm/zlink-perf-dotnet/perf-results/phase1-diag-routemesh-4096

bash framework/languages/dotnet/perf/scripts/run_diagnostic.sh \
  --scenario channel-echo-only --channel-topology routemesh \
  --logical-streams 16 --payload-size 4096 \
  --duration-seconds 30 --warmup-seconds 5 \
  --run-id phase1-diag-routemesh-4096-full \
  --output /dev/shm/zlink-perf-dotnet/perf-results/phase1-diag-routemesh-4096-full

# 5 connectors를 두 client에 3/2로 배분하는 실제 process 검증.
bash framework/languages/dotnet/perf/scripts/run_single.sh \
  --scenario session-echo-only --connections 5 --client-count 2 --connect-concurrency 2 \
  --payload-size 1024 --duration-seconds 1 --warmup-seconds 1 \
  --run-id phase1-contract-multiclient \
  --output /dev/shm/zlink-perf-dotnet/perf-results/phase1-contract-multiclient

bash framework/languages/dotnet/perf/scripts/collect_env.sh \
  /dev/shm/zlink-perf-dotnet/collect-env-report.json
```

## 셀별 결과

KOPS는 window 안에서 payload 검증까지 완료한 echo 수/owner seconds/1000이다. p50/p95/p99 단위는 ms이며 **nearest-rank bucket 상한의 추정값**이다. Settle은 `성공 echo 수 / window 종료 후 관찰 시간`으로 표시하며 KOPS에 더하지 않는다. 모든 완료된 측정 셀에서 cancelled와 unresolved는 0이다. `—`는 미측정이다.

일반 30초 실행(진단 Off, warmup 5초):

| 셀 | Payload bytes | 상태 | KOPS | p50 / p95 / p99 ms | 오류 | Settle 성공 / 시간 |
|---|---:|---|---:|---|---|---|
| session-echo-only | 1024 | valid | 1.4615 | 64 / 64 / 64 | 0 | 64 / 0.047554 s |
| session-echo-only | 4096 | valid | 1.4598 | 64 / 64 / 64 | 0 | 64 / 0.040309 s |
| channel-echo-only / RouteMesh | 1024 | valid | 17.651367 | 1 / 2 / 2 | 0 | 16 / 0.00199 s |
| channel-echo-only / RouteMesh | 4096 | failed: warmup | — | — / — / — | warmup DeadlineExceeded=16 | measured 미시작 |
| channel-echo-only / ClientServer | 1024 | 미실행: 공개 readiness blocker | — | — / — / — | smoke의 unsupported 참조 | — |
| channel-echo-only / ClientServer | 4096 | 미실행: 공개 readiness blocker | — | — / — / — | smoke의 unsupported 참조 | — |

Smoke 2초 실행(진단 Off, warmup 1초):

| 셀 | Payload bytes | 상태 | KOPS | p50 / p95 / p99 ms | 오류 | Settle 성공 / 시간 |
|---|---:|---|---:|---|---|---|
| session-echo-only | 1024 | valid | 1.438 | 64 / 64 / 64 | 0 | 64 / 0.036896 s |
| session-echo-only | 4096 | valid | 1.43 | 64 / 64 / 64 | 0 | 64 / 0.037655 s |
| channel-echo-only / RouteMesh | 1024 | valid | 11.4705 | 2 / 4 / 4 | 0 | 16 / 0.001324 s |
| channel-echo-only / RouteMesh | 4096 | failed | 5.944 | 2 / 4 / 4 | DeadlineExceeded=16 | 0 / 0.268096 s |
| channel-echo-only / ClientServer | 1024 | unsupported | — | — / — / — | 측정 미시작 | — |
| channel-echo-only / ClientServer | 4096 | unsupported | — | — / — / — | 측정 미시작 | — |

진단과 다중 client 검증은 일반 30초 기준값과 분리한다:

| 실행 | Payload bytes | 상태 | KOPS | p50 / p95 / p99 ms | 오류 | Settle 성공 / 시간 |
|---|---:|---|---:|---|---|---|
| RouteMesh / Normal tracing / 2초 | 4096 | valid | 9.9205 | 2 / 4 / 4 | 0 | 0 / 0.001209 s |
| RouteMesh / Normal tracing / 30초 | 4096 | valid | 13.990567 | 2 / 2 / 4 | 0 | 16 / 0.001494 s |
| Session / 2 clients / 1초 | 1024 | valid | 0.111 | 64 / 64 / 64 | 0 | 5 / 0.03355 s, 0.023527 s |

다중 client 결과는 전역 ID 0..4, 3/2 배분, 연결 5/5, 같은 resetSeq=1을 확인했다. `measuredSeconds=null+MULTIPLE_OWNERS`, throughput은 두 owner rate의 합이며 histogram은 count/sum/max를 합산했다. 진단 2개 결과는 모두 `baselineEligible=false`다.

## BLOCKERS

### B1. ClientServer Server-only의 공개 IsReady가 수신 가능 상태를 반영하지 않음

- 공개 exact interface: `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.ko.md:359`의 `ZLinkClientServerStatus`, `:363`의 `IsReady`, `:364`의 `ReadyTargetCount`, `:371`의 `GetStatus`; `:379`는 local/remote Ready Server를 target count에 포함한다고 명시한다.
- 소유 계약: `framework/doc/framework/common/spec/server/06-observability/01-runtime-monitoring.ko.md:174`의 topology 범위 및 `:194`의 ready 정의(Serving host에서 application message 처리 가능). Perf `README.ko.md:1351`/§16.1은 이 public 상태와 typed probe로 준비를 확인하도록 요구한다.
- 공개 재현: manual ClientServer source/target 두 process를 시작하면 target은 `host.State=Serving`, `host.IsReady=true`, `LocalRole=Server`, `ReadyTargetCount=1`, target `State=Ready`, weight 100이고 typed echo도 반환한다. 그러나 해당 topology는 `State=Degraded`, `IsReady=false`다. Source의 원본에는 `RequestToChannel.Async<PerfEchoReply>` 성공 evidence가 있다.
- 읽기 전용 원인 위치: `framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkClientServerRuntimeService.cs:98`의 `Selectable: state.HasClient && ...`와 `:131`의 public `IsReady` projection. Server-only는 HasClient 조건을 만족하지 못한다. 기존 Framework 결함(B)으로 보이며 수정하지 않았다. 새 API는 필요하지 않다.
- 처리: public readiness의 false를 무시하거나 Server-only에 Client 역할을 추가하지 않았다. 두 payload 셀을 `unsupported`, `baselineEligible=false`로 보존하고 30초 측정을 중단했다. Framework runtime 변경을 하지 않았으므로 변경 전 교차언어 구현 대조 및 runtime 구현 승인은 수행 대상이 아니다. 후속 runtime 수정은 별도 진단·승인 범위다.

- 1024 bytes 공개 증거: [request-ordinary-clientserver-sna-nna-ba353c289905445f4c92fe178a1ffdc29577b30190ce9500ba72e24cbe166b53/tmp/infrastructure-readiness-failed.json](/dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke/channel-echo-only/1024/request-ordinary-clientserver-sna-nna-ba353c289905445f4c92fe178a1ffdc29577b30190ce9500ba72e24cbe166b53/tmp/infrastructure-readiness-failed.json), [source snapshot](/dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke/channel-echo-only/1024/request-ordinary-clientserver-sna-nna-ba353c289905445f4c92fe178a1ffdc29577b30190ce9500ba72e24cbe166b53/server-channel-0.json), [target role config](/dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke/channel-echo-only/1024/request-ordinary-clientserver-sna-nna-ba353c289905445f4c92fe178a1ffdc29577b30190ce9500ba72e24cbe166b53/role-configs/channel-1.json).
- 4096 bytes 공개 증거: [request-ordinary-clientserver-sna-nna-b1c7260b97162a1373f45944bff2f3496d4afd975c51193d8287e217caafc1e1/tmp/infrastructure-readiness-failed.json](/dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke/channel-echo-only/4096/request-ordinary-clientserver-sna-nna-b1c7260b97162a1373f45944bff2f3496d4afd975c51193d8287e217caafc1e1/tmp/infrastructure-readiness-failed.json), [source snapshot](/dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke/channel-echo-only/4096/request-ordinary-clientserver-sna-nna-b1c7260b97162a1373f45944bff2f3496d4afd975c51193d8287e217caafc1e1/server-channel-0.json), [target role config](/dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke/channel-echo-only/4096/request-ordinary-clientserver-sna-nna-b1c7260b97162a1373f45944bff2f3496d4afd975c51193d8287e217caafc1e1/role-configs/channel-1.json).

이 차이를 우회해야 하는 다른 누락 API나 spec ambiguity는 확인하지 못했다. 위 두 셀은 §19/§22 성공 완료 수에 포함하지 않는다.

## 남은 실패와 해석 제한

RouteMesh 4096 일반 smoke는 sent=11904, completed=11888, timeout=16(`DeadlineExceeded`), failed/cancelled/unresolved=0이었다. [source 원본](/dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke/channel-echo-only/4096/request-ordinary-routemesh-sna-nna-00fbf9142615e83fad986a975fc95ed0c5433c6703a2947320d6300fca737b3c/server-channel-0.json)에 public 오류 kind와 첫 오류가 있다. 일반 30초 실행은 warmup sent=59126, completed=59110, timeout=16으로 실패했다. [warmup source 원본](/dev/shm/zlink-perf-dotnet/perf-results/phase1-full-routemesh-4096/channel-echo-only/4096/request-ordinary-routemesh-sna-nna-37f6e58c68f3085d9e52fd89b0d272f4d86cc01a5bb6aa31296338b481b0145f/server-channel-0.json)의 resetSeq는 0이며 measured reset/barrier를 열지 않았다. 같은 public 호출은 `framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/ChannelEchoOnlyScenario.cs:60`의 `RequestToChannel(...).Timeout(...).Async<PerfEchoReply>()`; exact 호출 선언은 `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/04-channel-messaging.ko.md:102`다. Framework/Core 중 어느 계층에서 deadline이 발생했는지는 확정하지 못했다.

기존 message-flow를 Normal로 켠 별도 2초·30초 실행은 오류를 재현하지 못했다. 두 실행의 source/target `logs/message-flow-channel-*.log`를 보존했고 이 수치를 무오류 일반 baseline으로 대체하지 않았다. Public status 조회를 application counter lock 밖으로 둔 harness 검토 수정 이후에도 일반 30초 warmup 실패가 남았으므로 이를 deadline 원인 해결이라고 주장하지 않는다. Timeout, workload, retry 횟수를 바꾸어 실패를 없앤 결과는 없다.

일반 30초 RouteMesh 4096의 기존 `summary.json`은 measured owner 부재로 metrics가 null이고 오류 map은 비어 있다. warmup의 실제 오류 16건은 위 raw source와 이 보고서에 보존했다. 최종 `results.py`는 같은 상황을 `PreMeasurementFailure.errorCounts`와 resetSeq로 summary에 함께 기록하며, 전용 regression test를 통과했다. 이전 실행 결과를 덮어쓰거나 다시 실행해 실패를 지우지 않았다.

소유 process handle로 종료를 확인했다. 다음 서버 3개는 SIGTERM 후 5초 내 종료하지 않아 동일 PID에 SIGKILL을 적용했다. 정상 graceful shutdown이 검증됐다고 해석하지 않는다. 이 처리는 report 이후의 자원 cleanup이며 처리량이나 deadline을 바꾸지 않았다.

| Run / 셀 | 서버 PID | exitCode | 근거 |
|---|---:|---:|---|
| phase1-smoke / channel-echo-only 4096 | 91811 | -9 | [cleanup.json](/dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke/channel-echo-only/4096/request-ordinary-routemesh-sna-nna-00fbf9142615e83fad986a975fc95ed0c5433c6703a2947320d6300fca737b3c/cleanup.json) |
| phase1-smoke / session-echo-only 4096 | 90677 | -9 | [cleanup.json](/dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke/session-echo-only/4096/request-ordinary-na-sna-nna-7797f480caf42b0ed48385b13d937dddfb7c383a55e239af51d1a524b1edee56/cleanup.json) |
| phase1-full-session / session-echo-only 1024 | 95677 | -9 | [cleanup.json](/dev/shm/zlink-perf-dotnet/perf-results/phase1-full-session/session-echo-only/1024/request-ordinary-na-sna-nna-3e57f44d260b8fe26c479f2bf07c131f637ff1a87adac7c2dd662bddb2311358/cleanup.json) |

Store가 없는 세 baseline 구성은 Docker Redis를 시작하지 않는다. `redis-common.sh`는 sample의 `redis-common.template.sh`를 source하고 run 이름·정확한 container ID·dotnet Redis port range(22000–22099)를 사용하도록 준비했지만, Redis 실행/종료와 Store namespace는 이번 phase의 실행 검증 대상이 아니었다. Host Redis fallback은 없다.

## 적용한 spec과 검증

| 조항 | Phase 1 구현 및 확인 |
|---|---|
| §1–§3, §6–§9, §17.1, §18.1–§18.2 | client/server/support 분리, Release role build, 명시적인 scenario body, shell entrypoints와 solution 등록 |
| §4/§4.1/§4.2, §5.2 | warmup → terminal drain → 같은 resetSeq acknowledgement → receiver/source 시작 → cohort window → bounded settle; HTTP trigger는 KOPS 제외 |
| §5/§5.1 | scenario별 consumer와 int32/finite/payload/mode/codec 검증, role config 선행 생성, child endpoint manifest 및 3/2 CS 분할 검증 |
| §11.1/§11.2 | public STREAM session echo, manual RouteMesh와 ClientServer source/target; payload 1024/4096; ClientServer readiness blocker를 보존 |
| §12/§13/§15.2 | typed JSON, canonical decimal text 및 전체 byte/identity 검증; Request completion 하나가 operation 결과; send/send 상관관계 기능은 비적용 |
| §14/§14.1/§14.2/§15.5 | 공통 metric key, 적용/미지원/표본 없음 구분, public error kind 보존, language/harness 별도 이름공간, runtime process 자료 |
| §15.1 | run/cell 디렉터리, SHA-256 variant, 실제 hash 입력 UTF-8 문자열, config/manifest/original/result/index 및 덮어쓰기 거부 |
| §15.3/§15.4 | histogram 정수 count/sum/max 집계, overflow null+reason, CS owner rate 합산과 Channel source 단일 owner, role application rate 별도 합산 |
| §16/§16.1–§16.3 | application HTTP ready/reset/stats와 별도 trigger listener; public capacity epoch 및 host/topology evidence; 측정 중 저비용 sampler, report에서 snapshot 직렬화 |
| §19/§20/§21 | 오류 셀 baseline 제외, packaged native의 실제 load/hash 검증, 프로세스별 자원·환경, OS 예약 port 및 소유 PID cleanup, 별도 tracing 진단 |
| §22의 phase 1 항목 | HTTP 거부/중복 요청, warmup active reset 거부, settle 분리, typed echo, 원본 집계·identity·격리 검증. 전체 matrix 성공은 B1과 RouteMesh 실패로 미완료 |

의도적으로 채택한 계약 이탈은 없다. 완료되지 않은 항목은 ClientServer 2개 셀의 공개 상태 계약, 일반 RouteMesh 4096의 30초 measured 결과, 그리고 강제 종료 3건의 정상 shutdown이다. 향후 Actor/Spot/Worker/PubSub/Store 검증은 사용자 지정 phase 1 범위 밖이다.

```bash
source framework/languages/dotnet/perf/scripts/dotnet-env.sh
flock --exclusive --close /tmp/zlink-dotnet-gate.lock \
  dotnet test framework/languages/dotnet/perf/ZLink.Framework.Perf.Tests \
  -c Release -m:1 --nologo
python3 framework/languages/dotnet/perf/scripts/test_harness.py
python3 scripts/verify-framework-runner-isolation.py
for script in framework/languages/dotnet/perf/scripts/*.sh; do bash -n "$script" || exit; done
python3 -m py_compile framework/languages/dotnet/perf/scripts/*.py

# 실행 결과를 읽기만 하는 scratch 검증. 출력 파일은 새 경로에서 한 번 생성한다.
python3 /dev/shm/zlink-perf-dotnet/audit-results.py
# application HTTP 계약 검증은 소유 process와 OS 예약 port를 사용한다.
flock --exclusive --close /tmp/zlink-samples-gate.lock \
  python3 /dev/shm/zlink-perf-dotnet/test-admin-contract.py
```

- .NET 관련 test: **11 passed / 0 failed** — [log](/dev/shm/zlink-perf-dotnet/harness-tests-final.log).
- Python harness test: **9 passed / 0 failed** — [log](/dev/shm/zlink-perf-dotnet/python-harness-tests-final.log).
- 실제 HTTP 계약: **16 cases passed**, 잘못된 입력의 state 보존 확인 — [evidence](/dev/shm/zlink-perf-dotnet/admin-contract/http-contract-checks.json).
- 저장 결과 대조: **13 cells / 6,707 checks passed** — [audit](/dev/shm/zlink-perf-dotnet/artifact-audit.json). 이는 실패/unsupported 셀의 원본 보존을 포함한 자료 정합성 검사이며 13개 성능 성공을 뜻하지 않는다.
- runner isolation: `FRAMEWORK RUNNER ISOLATION CLEAN ranges=20 locks=5 runners=76 sample_runners=67 redis_helpers=7` — [log](/dev/shm/zlink-perf-dotnet/runner-isolation-verification.log).
- role Release build, shell syntax, Python compile, 해당 solution diff의 whitespace 검사를 통과했다. 전체 solution/test 및 다른 언어 gate는 실행하지 않았다. 기존 Framework 소스에서 관찰한 `ZLinkSpotNodeCatalog.cs:768`의 CS8619 warning은 범위 밖이므로 수정하지 않았다.

`scripts/verify-framework-runner-isolation.py`는 `:689`의 `*/run_sample.*`와 `:754` 부근의 지정된 E2E runner를 inventory로 검사한다. 새 perf runner는 이 inventory에 포함되지 않으며 새 고정 listener range도 요구하지 않는다. 따라서 해당 script를 수정하지 않았다. 이번 runner의 실제 port 예약·공유 lock·소유 PID cleanup은 코드와 run evidence로 별도 확인했다.

## 환경과 자원

환경: `Intel(R) Core(TM) Ultra 7 265K`, effective processors=20, `6.6.87.2-microsoft-standard-WSL2`, 같은 host의 loopback. CPU quota/memory limit은 수집된 cgroup에서 별도 상한을 확인하지 못해 null이고 affinity/cpuset은 0–19다. CPU는 1 core=100% 단위이며 process별 값을 합쳐 단일 owner resource 값으로 만들지 않는다. .NET 실제 runtime은 8.0.30이며 Release, Framework 0.10.0, binding/Core 0.17.0, 시작 commit `79094d9f1f87bf561d08f9b38c17d1e0e69b77f4`의 dirty checkout이다. 아래 원본에 정확한 runtime/SDK 목록과 artifact hash를 복사했다.

| 일반 30초 셀 | Process | CPU % (1 core=100) | RSS max MiB | 할당 MiB |
|---|---|---:|---:|---:|
| session-echo-only 4096 | client-0.json | 15.23 | 91.18 | 1216.58 |
| session-echo-only 4096 | server-session-0.json | 35.07 | 527.19 | 1739.34 |
| session-echo-only 1024 | client-0.json | 10.63 | 86.66 | 507.67 |
| session-echo-only 1024 | server-session-0.json | 27.03 | 495.31 | 844.30 |
| channel-echo-only 1024 | server-channel-1.json | 619.77 | 538.87 | 18279.88 |
| channel-echo-only 1024 | server-channel-0.json | 489.24 | 1917.66 | 10975.49 |

Session client의 window 평균 CPU는 10.63–15.23%로 이 관측만으로 client CPU 포화를 주장할 수 없다. RouteMesh는 source 약 4.89 cores, target 약 6.20 cores에 해당하는 CPU를 사용했으며 loopback의 CPU 경쟁을 포함한다. 짧은 순간의 포화·개별 runtime lane 병목 여부는 평균 process 지표로 판정하지 않는다. RSS는 요청 간격 100ms sampler의 최댓값이며 실제 sample 간격을 원본에 기록한다. Framework host capacity/PAUSED/ready 관측을 보존하며 Spot mailbox나 내부 queue 지표로 바꾸지 않는다.

지정된 `ZLINK_LIBRARY_PATH`는 directory다. `/proc/<owned-pid>/maps`에서 실제 확인한 native load 위치는 perf server 출력의 `runtimes/linux-x64/native/libzlink.so`였다. 이 packaged 파일의 SHA-256은 승인된 `core/build-dev/lib/libzlink.so`와 동일한 `64567f1715b3f1527afbc1c290e2b262d02d722768e160227a6f9815bdd4bb43`다. 실제 경로를 local Core directory로 바꾸어 표기하지 않았다. 최종 runner는 build 후 두 서버 payload hash가 이 local Core와 다르면 실행 전에 실패한다. 정상 phase를 완료한 셀의 `loaded-artifacts.json`에 PID별 실제 경로를 보존했다. Setup/warmup에서 중단한 셀은 run env artifact hash만 있고 이 추가 load snapshot이 없으므로 정상 측정 provenance로 채택하지 않는다.

Serialized message byte 수를 얻는 public per-message 관측은 사용하지 않았으므로 `observedSerializedBytes=null+PUBLIC_OBSERVATION_UNSUPPORTED`다. Payload bytes는 Base64/JSON/header를 제외한 logical bytes다. RTT는 source의 동일 Stopwatch domain만 사용하며 cross-process 정확한 시작 차이는 `CLOCK_DOMAIN_UNVERIFIED`다. Coordinator trigger send/ack로 계산한 관찰 bound는 각 셀의 `tmp/measured-start-barrier.json`에 있다.

## 원본 summary.json 사본

아래 JSON은 scratch의 summary.json을 그대로 복사했다. 본문 표는 명시한 phase1-smoke/full/diagnostic/multiclient run을 사용한다. `phase1-dev-*`는 초기 구현 확인 자료로 보존한 것이며 본문 성능 기준값에 포함하지 않는다. 각 사본의 source와 SHA-256으로 원본 대조가 가능하다. 원본 result.json·config.json·snapshot·flow log는 해당 셀 디렉터리에 유지한다.

### phase1-contract-multiclient · session-echo-only · 1024 · STREAM

Source: [/dev/shm/zlink-perf-dotnet/perf-results/phase1-contract-multiclient/session-echo-only/1024/request-ordinary-na-sna-nna-95d0317ad66b29b5d4df50afe1489e046e3f653e8f6374b6ec59aa348078d494/summary.json](/dev/shm/zlink-perf-dotnet/perf-results/phase1-contract-multiclient/session-echo-only/1024/request-ordinary-na-sna-nna-95d0317ad66b29b5d4df50afe1489e046e3f653e8f6374b6ec59aa348078d494/summary.json)  
SHA-256: `27001530cd642357bc8e080278cddae65c5079c962345d9556797a894743d905`

```json
{
  "schemaVersion": 2,
  "runId": "phase1-contract-multiclient",
  "cellId": "session-echo-only/1024/request-ordinary-na-sna-nna-95d0317ad66b29b5d4df50afe1489e046e3f653e8f6374b6ec59aa348078d494",
  "scenario": "session-echo-only",
  "status": "valid",
  "baselineEligible": true,
  "reasons": [],
  "metricOwners": [
    "client-0.json",
    "client-1.json"
  ],
  "ownerWindows": {
    "client-0.json": {
      "startedAtUnixMs": "1788673175639",
      "endedAtUnixMs": "1788673176640",
      "startTicks": "151112646521248",
      "endTicks": "151113646521248",
      "measuredSeconds": 1,
      "settleSeconds": 0.033549895
    },
    "client-1.json": {
      "startedAtUnixMs": "1788673175645",
      "endedAtUnixMs": "1788673176650",
      "startTicks": "151112652128636",
      "endTicks": "151113652128636",
      "measuredSeconds": 1,
      "settleSeconds": 0.023526902
    }
  },
  "payloadSize": 1024,
  "topology": null,
  "diagnostics": "Off",
  "metrics": {
    "messages.admitted": null,
    "messages.expired": null,
    "messages.duplicateReply": null,
    "messages.lateReply": null,
    "messages.unknownCorrelation": null,
    "messages.published": null,
    "messages.publishedInWindow": null,
    "messages.settlePublished": null,
    "messages.sent": "116",
    "messages.completed": "111",
    "messages.settleCompleted": "5",
    "messages.failed": "0",
    "messages.timeout": "0",
    "messages.cancelled": "0",
    "messages.unresolved": "0",
    "latency.meanMs": 44.11037845045045,
    "latency.p50Ms": 64,
    "latency.p95Ms": 64,
    "latency.p99Ms": 64,
    "latency.maxMs": 60.078967,
    "settle.latency.meanMs": 45.443370200000004,
    "settle.latency.p50Ms": 64,
    "settle.latency.p95Ms": 64,
    "settle.latency.p99Ms": 64,
    "settle.latency.maxMs": 47.529203,
    "connections.requested": "5",
    "connections.connected": "5",
    "connections.failed": "0",
    "throughput.kops": 0.111,
    "throughput.messagesPerSec": 227,
    "throughput.megabytesPerSec": 0.2216796875,
    "errors.byKind": {},
    "errors.harness": {},
    "errors.language": {}
  },
  "processes": [
    {
      "sourceFile": "client-0.json",
      "pid": 40005,
      "resources": {
        "process.cpuPercent": 25.99988836643638,
        "process.rssMb": 64.53125,
        "process.allocatedMb": 1.1399612426757812,
        "gc.gen0": "0",
        "gc.gen1": "0",
        "gc.gen2": "0"
      }
    },
    {
      "sourceFile": "client-1.json",
      "pid": 40006,
      "resources": {
        "process.cpuPercent": 40.81922670720843,
        "process.rssMb": 64.53125,
        "process.allocatedMb": 0.8563308715820312,
        "gc.gen0": "0",
        "gc.gen1": "0",
        "gc.gen2": "0"
      }
    },
    {
      "sourceFile": "server-session-0.json",
      "pid": 39945,
      "resources": {
        "process.cpuPercent": 38.96392641764402,
        "process.rssMb": 136.25,
        "process.allocatedMb": 3.13916015625,
        "gc.gen0": "0",
        "gc.gen1": "0",
        "gc.gen2": "0"
      }
    }
  ],
  "limitations": [
    "Percentiles are nearest-rank bucket upper-bound estimates, not exact observations.",
    "Same-host loopback includes CPU competition. Process CPU is one-core=100%.",
    "Unobservable internal metrics and serialized byte sizes remain null with reasons in result.json."
  ],
  "nullReasons": {}
}
```

### phase1-dev-clientserver · channel-echo-only · 4096 · clientserver

Source: [/dev/shm/zlink-perf-dotnet/perf-results/phase1-dev-clientserver/channel-echo-only/4096/request-ordinary-clientserver-sna-nna-898ca21ad6db8c6ebb213d672d121545dec66583120d406aa158cca77d94881f/summary.json](/dev/shm/zlink-perf-dotnet/perf-results/phase1-dev-clientserver/channel-echo-only/4096/request-ordinary-clientserver-sna-nna-898ca21ad6db8c6ebb213d672d121545dec66583120d406aa158cca77d94881f/summary.json)  
SHA-256: `debe138b603a8ff9dc33bae317c2689e2dbdaad062b4df1059b9e63b5e7603ac`

```json
{
  "schemaVersion": 2,
  "runId": "phase1-dev-clientserver",
  "cellId": "channel-echo-only/4096/request-ordinary-clientserver-sna-nna-898ca21ad6db8c6ebb213d672d121545dec66583120d406aa158cca77d94881f",
  "scenario": "channel-echo-only",
  "status": "failed",
  "baselineEligible": false,
  "reasons": [
    {
      "code": "CollectionFailure",
      "message": "TimeoutError: Public readiness evidence did not converge inside setupTimeoutMs=30000",
      "sourceFile": "logs/"
    },
    {
      "code": "CollectionFailure",
      "message": "[Errno 2] No such file or directory: '/dev/shm/zlink-perf-dotnet/perf-results/phase1-dev-clientserver/channel-echo-only/4096/request-ordinary-clientserver-sna-nna-898ca21ad6db8c6ebb213d672d121545dec66583120d406aa158cca77d94881f/client-0.json'",
      "sourceFile": "client-0.json"
    },
    {
      "code": "CollectionFailure",
      "message": "PhaseMismatch: original has no completed measured reset epoch",
      "sourceFile": "server-channel-1.json"
    },
    {
      "code": "CollectionFailure",
      "message": "PhaseMismatch: original has no completed measured reset epoch",
      "sourceFile": "server-channel-0.json"
    },
    {
      "code": "CollectionFailure",
      "message": "Missing primary owner original.",
      "sourceFile": "server-channel-0.json"
    }
  ],
  "metricOwners": [
    "server-channel-0.json"
  ],
  "ownerWindows": {},
  "payloadSize": 4096,
  "topology": "clientserver",
  "metrics": {},
  "processes": [],
  "limitations": [
    "Percentiles are nearest-rank bucket upper-bound estimates, not exact observations.",
    "Same-host loopback includes CPU competition. Process CPU is one-core=100%.",
    "Unobservable internal metrics and serialized byte sizes remain null with reasons in result.json."
  ],
  "nullReasons": {}
}
```

### phase1-dev-routemesh · channel-echo-only · 4096 · routemesh

Source: [/dev/shm/zlink-perf-dotnet/perf-results/phase1-dev-routemesh/channel-echo-only/4096/request-ordinary-routemesh-sna-nna-dc2e6580281c565eea59e60b5c857a6129da763845a45213a0bd9ff78cee6afc/summary.json](/dev/shm/zlink-perf-dotnet/perf-results/phase1-dev-routemesh/channel-echo-only/4096/request-ordinary-routemesh-sna-nna-dc2e6580281c565eea59e60b5c857a6129da763845a45213a0bd9ff78cee6afc/summary.json)  
SHA-256: `a7f1ce3550d7d6ff2f877822f9bdc465615e51d9294f739d9e9b01c0a49b77b5`

```json
{
  "schemaVersion": 2,
  "runId": "phase1-dev-routemesh",
  "cellId": "channel-echo-only/4096/request-ordinary-routemesh-sna-nna-dc2e6580281c565eea59e60b5c857a6129da763845a45213a0bd9ff78cee6afc",
  "scenario": "channel-echo-only",
  "status": "failed",
  "baselineEligible": false,
  "reasons": [
    {
      "code": "CollectionFailure",
      "message": "TimeoutError: Public readiness evidence did not converge inside setupTimeoutMs=30000",
      "sourceFile": "logs/"
    },
    {
      "code": "CollectionFailure",
      "message": "[Errno 2] No such file or directory: '/dev/shm/zlink-perf-dotnet/perf-results/phase1-dev-routemesh/channel-echo-only/4096/request-ordinary-routemesh-sna-nna-dc2e6580281c565eea59e60b5c857a6129da763845a45213a0bd9ff78cee6afc/client-0.json'",
      "sourceFile": "client-0.json"
    },
    {
      "code": "CollectionFailure",
      "message": "PhaseMismatch: original has no completed measured reset epoch",
      "sourceFile": "server-channel-1.json"
    },
    {
      "code": "CollectionFailure",
      "message": "PhaseMismatch: original has no completed measured reset epoch",
      "sourceFile": "server-channel-0.json"
    },
    {
      "code": "CollectionFailure",
      "message": "Missing primary owner original.",
      "sourceFile": "server-channel-0.json"
    }
  ],
  "metricOwners": [
    "server-channel-0.json"
  ],
  "ownerWindows": {},
  "payloadSize": 4096,
  "topology": "routemesh",
  "metrics": {},
  "processes": [],
  "limitations": [
    "Percentiles are nearest-rank bucket upper-bound estimates, not exact observations.",
    "Same-host loopback includes CPU competition. Process CPU is one-core=100%.",
    "Unobservable internal metrics and serialized byte sizes remain null with reasons in result.json."
  ],
  "nullReasons": {}
}
```

### phase1-dev-routemesh-v2 · channel-echo-only · 4096 · routemesh

Source: [/dev/shm/zlink-perf-dotnet/perf-results/phase1-dev-routemesh-v2/channel-echo-only/4096/request-ordinary-routemesh-sna-nna-dc2e6580281c565eea59e60b5c857a6129da763845a45213a0bd9ff78cee6afc/summary.json](/dev/shm/zlink-perf-dotnet/perf-results/phase1-dev-routemesh-v2/channel-echo-only/4096/request-ordinary-routemesh-sna-nna-dc2e6580281c565eea59e60b5c857a6129da763845a45213a0bd9ff78cee6afc/summary.json)  
SHA-256: `9965613d5e00482c4d98f101b81296616b8dcbbb6bc00ee1cfa7718fdb1b03e9`

```json
{
  "schemaVersion": 2,
  "runId": "phase1-dev-routemesh-v2",
  "cellId": "channel-echo-only/4096/request-ordinary-routemesh-sna-nna-dc2e6580281c565eea59e60b5c857a6129da763845a45213a0bd9ff78cee6afc",
  "scenario": "channel-echo-only",
  "status": "valid",
  "baselineEligible": true,
  "reasons": [],
  "metricOwners": [
    "server-channel-0.json"
  ],
  "ownerWindows": {
    "server-channel-0.json": {
      "startedAtUnixMs": "1788670422502",
      "endedAtUnixMs": "1788670423504",
      "startTicks": "148359215718944",
      "endTicks": "148360215718944",
      "measuredSeconds": 1,
      "settleSeconds": 0.002039672
    }
  },
  "payloadSize": 4096,
  "topology": "routemesh",
  "metrics": {
    "messages.admitted": null,
    "messages.expired": null,
    "messages.duplicateReply": null,
    "messages.lateReply": null,
    "messages.unknownCorrelation": null,
    "messages.published": null,
    "messages.publishedInWindow": null,
    "messages.settlePublished": null,
    "messages.sent": "3503",
    "messages.completed": "3499",
    "messages.settleCompleted": "4",
    "messages.failed": "0",
    "messages.timeout": "0",
    "messages.cancelled": "0",
    "messages.unresolved": "0",
    "latency.meanMs": 1.1307885713060875,
    "latency.p50Ms": 2,
    "latency.p95Ms": 2,
    "latency.p99Ms": 2,
    "latency.maxMs": 10.294386,
    "settle.latency.meanMs": 1.10881875,
    "settle.latency.p50Ms": 2,
    "settle.latency.p95Ms": 2,
    "settle.latency.p99Ms": 2,
    "settle.latency.maxMs": 1.184345,
    "connections.requested": null,
    "connections.connected": null,
    "connections.failed": null,
    "throughput.kops": 3.499,
    "throughput.messagesPerSec": 6990,
    "throughput.megabytesPerSec": 27.3046875,
    "errors.byKind": {},
    "errors.harness": {},
    "errors.language": {}
  },
  "processes": [
    {
      "sourceFile": "client-0.json",
      "pid": 62194,
      "resources": {
        "process.cpuPercent": 13.996307060385812,
        "process.rssMb": 55.625,
        "process.allocatedMb": 0.28836822509765625,
        "gc.gen0": "0",
        "gc.gen1": "0",
        "gc.gen2": "0"
      }
    },
    {
      "sourceFile": "server-channel-1.json",
      "pid": 62010,
      "resources": {
        "process.cpuPercent": 374.37843670295024,
        "process.rssMb": 472.8125,
        "process.allocatedMb": 208.0653076171875,
        "gc.gen0": "1",
        "gc.gen1": "0",
        "gc.gen2": "0"
      }
    },
    {
      "sourceFile": "server-channel-0.json",
      "pid": 62011,
      "resources": {
        "process.cpuPercent": 242.61367169374176,
        "process.rssMb": 489.53125,
        "process.allocatedMb": 160.87117767333984,
        "gc.gen0": "0",
        "gc.gen1": "0",
        "gc.gen2": "0"
      }
    }
  ],
  "limitations": [
    "Percentiles are nearest-rank bucket upper-bound estimates, not exact observations.",
    "Same-host loopback includes CPU competition. Process CPU is one-core=100%.",
    "Unobservable internal metrics and serialized byte sizes remain null with reasons in result.json."
  ],
  "nullReasons": {}
}
```

### phase1-dev-session · session-echo-only · 1024 · STREAM

Source: [/dev/shm/zlink-perf-dotnet/perf-results/phase1-dev-session/session-echo-only/1024/request-ordinary-na-sna-nna-4959060dc87dcfd8005a0bcb0ac34836626d004aff2977b53cb08d32ff63abce/summary.json](/dev/shm/zlink-perf-dotnet/perf-results/phase1-dev-session/session-echo-only/1024/request-ordinary-na-sna-nna-4959060dc87dcfd8005a0bcb0ac34836626d004aff2977b53cb08d32ff63abce/summary.json)  
SHA-256: `32726503c2544173c386c884f3f3c1f59b3f4279b22a0ce0ac56daf541709269`

```json
{
  "schemaVersion": 2,
  "runId": "phase1-dev-session",
  "cellId": "session-echo-only/1024/request-ordinary-na-sna-nna-4959060dc87dcfd8005a0bcb0ac34836626d004aff2977b53cb08d32ff63abce",
  "scenario": "session-echo-only",
  "status": "valid",
  "baselineEligible": true,
  "reasons": [],
  "metricOwners": [
    "client-0.json"
  ],
  "ownerWindows": {
    "client-0.json": {
      "startedAtUnixMs": "1788670233923",
      "endedAtUnixMs": "1788670234923",
      "startTicks": "148170625104853",
      "endTicks": "148171625104853",
      "measuredSeconds": 1,
      "settleSeconds": 0.003327804
    }
  },
  "payloadSize": 1024,
  "topology": null,
  "metrics": {
    "messages.admitted": null,
    "messages.expired": null,
    "messages.duplicateReply": null,
    "messages.lateReply": null,
    "messages.unknownCorrelation": null,
    "messages.published": null,
    "messages.publishedInWindow": null,
    "messages.settlePublished": null,
    "messages.sent": "184",
    "messages.completed": "176",
    "messages.settleCompleted": "8",
    "messages.failed": "0",
    "messages.timeout": "0",
    "messages.cancelled": "0",
    "messages.unresolved": "0",
    "latency.meanMs": 43.434723795454545,
    "latency.p50Ms": 64,
    "latency.p95Ms": 64,
    "latency.p99Ms": 64,
    "latency.maxMs": 48.469951,
    "settle.latency.meanMs": 44.398514,
    "settle.latency.p50Ms": 64,
    "settle.latency.p95Ms": 64,
    "settle.latency.p99Ms": 64,
    "settle.latency.maxMs": 44.565041,
    "connections.requested": "8",
    "connections.connected": "8",
    "connections.failed": "0",
    "throughput.kops": 0.176,
    "throughput.messagesPerSec": 360,
    "throughput.megabytesPerSec": 0.3515625,
    "errors.byKind": {},
    "errors.harness": {},
    "errors.language": {}
  },
  "processes": [
    {
      "sourceFile": "client-0.json",
      "pid": 53118,
      "resources": {
        "process.cpuPercent": 38.00137276158964,
        "process.rssMb": 71.09375,
        "process.allocatedMb": 2.6989212036132812,
        "gc.gen0": "0",
        "gc.gen1": "0",
        "gc.gen2": "0"
      }
    },
    {
      "sourceFile": "server-session-0.json",
      "pid": 53063,
      "resources": {
        "process.cpuPercent": 48.898019756935525,
        "process.rssMb": 140.15625,
        "process.allocatedMb": 4.472450256347656,
        "gc.gen0": "0",
        "gc.gen1": "0",
        "gc.gen2": "0"
      }
    }
  ],
  "limitations": [
    "Percentiles are nearest-rank bucket upper-bound estimates, not exact observations.",
    "Same-host loopback includes CPU competition. Process CPU is one-core=100%.",
    "Unobservable internal metrics and serialized byte sizes remain null with reasons in result.json."
  ],
  "nullReasons": {}
}
```

### phase1-diag-routemesh-4096 · channel-echo-only · 4096 · routemesh

Source: [/dev/shm/zlink-perf-dotnet/perf-results/phase1-diag-routemesh-4096/channel-echo-only/4096/request-ordinary-routemesh-sna-nna-9019b7e1a4eba21d0df96853412f246cb982776d863e5338af1433586234f52b/summary.json](/dev/shm/zlink-perf-dotnet/perf-results/phase1-diag-routemesh-4096/channel-echo-only/4096/request-ordinary-routemesh-sna-nna-9019b7e1a4eba21d0df96853412f246cb982776d863e5338af1433586234f52b/summary.json)  
SHA-256: `ff583e422885671f50caedcc412c58f7d49e0d0a08e91b62a4eca9de5f22ed70`

```json
{
  "schemaVersion": 2,
  "runId": "phase1-diag-routemesh-4096",
  "cellId": "channel-echo-only/4096/request-ordinary-routemesh-sna-nna-9019b7e1a4eba21d0df96853412f246cb982776d863e5338af1433586234f52b",
  "scenario": "channel-echo-only",
  "status": "valid",
  "baselineEligible": false,
  "reasons": [],
  "metricOwners": [
    "server-channel-0.json"
  ],
  "ownerWindows": {
    "server-channel-0.json": {
      "startedAtUnixMs": "1788671628659",
      "endedAtUnixMs": "1788671630660",
      "startTicks": "149565507561342",
      "endTicks": "149567507561342",
      "measuredSeconds": 2,
      "settleSeconds": 0.001209203
    }
  },
  "payloadSize": 4096,
  "topology": "routemesh",
  "diagnostics": "Normal",
  "metrics": {
    "messages.admitted": null,
    "messages.expired": null,
    "messages.duplicateReply": null,
    "messages.lateReply": null,
    "messages.unknownCorrelation": null,
    "messages.published": null,
    "messages.publishedInWindow": null,
    "messages.settlePublished": null,
    "messages.sent": "19841",
    "messages.completed": "19841",
    "messages.settleCompleted": "0",
    "messages.failed": "0",
    "messages.timeout": "0",
    "messages.cancelled": "0",
    "messages.unresolved": "0",
    "latency.meanMs": 1.5983326985030997,
    "latency.p50Ms": 2,
    "latency.p95Ms": 4,
    "latency.p99Ms": 4,
    "latency.maxMs": 42.445206,
    "settle.latency.meanMs": null,
    "settle.latency.p50Ms": null,
    "settle.latency.p95Ms": null,
    "settle.latency.p99Ms": null,
    "settle.latency.maxMs": null,
    "connections.requested": null,
    "connections.connected": null,
    "connections.failed": null,
    "throughput.kops": 9.9205,
    "throughput.messagesPerSec": 19833.0,
    "throughput.megabytesPerSec": 77.47265625,
    "errors.byKind": {},
    "errors.harness": {},
    "errors.language": {}
  },
  "processes": [
    {
      "sourceFile": "client-0.json",
      "pid": 94302,
      "resources": {
        "process.cpuPercent": 9.998819689329773,
        "process.rssMb": 55.625,
        "process.allocatedMb": 0.5373001098632812,
        "gc.gen0": "0",
        "gc.gen1": "0",
        "gc.gen2": "0"
      }
    },
    {
      "sourceFile": "server-channel-1.json",
      "pid": 94196,
      "resources": {
        "process.cpuPercent": 778.3557869588354,
        "process.rssMb": 517.94921875,
        "process.allocatedMb": 1333.1729202270508,
        "gc.gen0": "4",
        "gc.gen1": "1",
        "gc.gen2": "0"
      }
    },
    {
      "sourceFile": "server-channel-0.json",
      "pid": 94197,
      "resources": {
        "process.cpuPercent": 601.8885215184312,
        "process.rssMb": 701.09375,
        "process.allocatedMb": 1055.567237854004,
        "gc.gen0": "3",
        "gc.gen1": "1",
        "gc.gen2": "0"
      }
    }
  ],
  "limitations": [
    "Percentiles are nearest-rank bucket upper-bound estimates, not exact observations.",
    "Same-host loopback includes CPU competition. Process CPU is one-core=100%.",
    "Unobservable internal metrics and serialized byte sizes remain null with reasons in result.json."
  ],
  "nullReasons": {
    "/metrics/settle.latency.meanMs": {
      "code": "NO_SAMPLES",
      "reason": "No successful samples.",
      "owner": "perf/README.ko.md §15.3"
    },
    "/metrics/settle.latency.p50Ms": {
      "code": "NO_SAMPLES",
      "reason": "No successful samples.",
      "owner": "perf/README.ko.md §15.3"
    },
    "/metrics/settle.latency.p95Ms": {
      "code": "NO_SAMPLES",
      "reason": "No successful samples.",
      "owner": "perf/README.ko.md §15.3"
    },
    "/metrics/settle.latency.p99Ms": {
      "code": "NO_SAMPLES",
      "reason": "No successful samples.",
      "owner": "perf/README.ko.md §15.3"
    },
    "/metrics/settle.latency.maxMs": {
      "code": "NO_SAMPLES",
      "reason": "No successful samples.",
      "owner": "perf/README.ko.md §15.3"
    }
  }
}
```

### phase1-diag-routemesh-4096-full · channel-echo-only · 4096 · routemesh

Source: [/dev/shm/zlink-perf-dotnet/perf-results/phase1-diag-routemesh-4096-full/channel-echo-only/4096/request-ordinary-routemesh-sna-nna-5cb934ec7c5aaac76c096fcb2e71b9926929cbafb032ee4c91d636d11b821b2f/summary.json](/dev/shm/zlink-perf-dotnet/perf-results/phase1-diag-routemesh-4096-full/channel-echo-only/4096/request-ordinary-routemesh-sna-nna-5cb934ec7c5aaac76c096fcb2e71b9926929cbafb032ee4c91d636d11b821b2f/summary.json)  
SHA-256: `0101b0244e1cf648ccad273d868f475ae57c56d5750321838f80ce6be189d43b`

```json
{
  "schemaVersion": 2,
  "runId": "phase1-diag-routemesh-4096-full",
  "cellId": "channel-echo-only/4096/request-ordinary-routemesh-sna-nna-5cb934ec7c5aaac76c096fcb2e71b9926929cbafb032ee4c91d636d11b821b2f",
  "scenario": "channel-echo-only",
  "status": "valid",
  "baselineEligible": false,
  "reasons": [],
  "metricOwners": [
    "server-channel-0.json"
  ],
  "ownerWindows": {
    "server-channel-0.json": {
      "startedAtUnixMs": "1788672693382",
      "endedAtUnixMs": "1788672723384",
      "startTicks": "150630344983829",
      "endTicks": "150660344983829",
      "measuredSeconds": 30,
      "settleSeconds": 0.001493616
    }
  },
  "payloadSize": 4096,
  "topology": "routemesh",
  "diagnostics": "Normal",
  "metrics": {
    "messages.admitted": null,
    "messages.expired": null,
    "messages.duplicateReply": null,
    "messages.lateReply": null,
    "messages.unknownCorrelation": null,
    "messages.published": null,
    "messages.publishedInWindow": null,
    "messages.settlePublished": null,
    "messages.sent": "419733",
    "messages.completed": "419717",
    "messages.settleCompleted": "16",
    "messages.failed": "0",
    "messages.timeout": "0",
    "messages.cancelled": "0",
    "messages.unresolved": "0",
    "latency.meanMs": 1.1376108967327985,
    "latency.p50Ms": 2,
    "latency.p95Ms": 2,
    "latency.p99Ms": 4,
    "latency.maxMs": 25.977099,
    "settle.latency.meanMs": 1.322801375,
    "settle.latency.p50Ms": 2,
    "settle.latency.p95Ms": 2,
    "settle.latency.p99Ms": 2,
    "settle.latency.maxMs": 1.604866,
    "connections.requested": null,
    "connections.connected": null,
    "connections.failed": null,
    "throughput.kops": 13.990566666666668,
    "throughput.messagesPerSec": 27980.800000000003,
    "throughput.megabytesPerSec": 109.30000000000001,
    "errors.byKind": {},
    "errors.harness": {},
    "errors.language": {}
  },
  "processes": [
    {
      "sourceFile": "client-0.json",
      "pid": 19092,
      "resources": {
        "process.cpuPercent": 1.0333348397977515,
        "process.rssMb": 62.96875,
        "process.allocatedMb": 7.0343017578125,
        "gc.gen0": "0",
        "gc.gen1": "0",
        "gc.gen2": "0"
      }
    },
    {
      "sourceFile": "server-channel-1.json",
      "pid": 18629,
      "resources": {
        "process.cpuPercent": 653.605818293119,
        "process.rssMb": 559.66015625,
        "process.allocatedMb": 28207.26905822754,
        "gc.gen0": "79",
        "gc.gen1": "4",
        "gc.gen2": "1"
      }
    },
    {
      "sourceFile": "server-channel-0.json",
      "pid": 18636,
      "resources": {
        "process.cpuPercent": 524.283604882726,
        "process.rssMb": 3630.3203125,
        "process.allocatedMb": 22302.34497833252,
        "gc.gen0": "62",
        "gc.gen1": "8",
        "gc.gen2": "0"
      }
    }
  ],
  "limitations": [
    "Percentiles are nearest-rank bucket upper-bound estimates, not exact observations.",
    "Same-host loopback includes CPU competition. Process CPU is one-core=100%.",
    "Unobservable internal metrics and serialized byte sizes remain null with reasons in result.json."
  ],
  "nullReasons": {}
}
```

### phase1-full-routemesh-1024 · channel-echo-only · 1024 · routemesh

Source: [/dev/shm/zlink-perf-dotnet/perf-results/phase1-full-routemesh-1024/channel-echo-only/1024/request-ordinary-routemesh-sna-nna-b0ed7f7c21864b9fceccab1829e0db5893eca71661d6bf7be6d60d5eb6294321/summary.json](/dev/shm/zlink-perf-dotnet/perf-results/phase1-full-routemesh-1024/channel-echo-only/1024/request-ordinary-routemesh-sna-nna-b0ed7f7c21864b9fceccab1829e0db5893eca71661d6bf7be6d60d5eb6294321/summary.json)  
SHA-256: `d72190e51cb7fd87f68b19facd72b6229c99064488b09296e2e11eeb942572b2`

```json
{
  "schemaVersion": 2,
  "runId": "phase1-full-routemesh-1024",
  "cellId": "channel-echo-only/1024/request-ordinary-routemesh-sna-nna-b0ed7f7c21864b9fceccab1829e0db5893eca71661d6bf7be6d60d5eb6294321",
  "scenario": "channel-echo-only",
  "status": "valid",
  "baselineEligible": true,
  "reasons": [],
  "metricOwners": [
    "server-channel-0.json"
  ],
  "ownerWindows": {
    "server-channel-0.json": {
      "startedAtUnixMs": "1788672252450",
      "endedAtUnixMs": "1788672282448",
      "startTicks": "150189361171816",
      "endTicks": "150219361171816",
      "measuredSeconds": 30,
      "settleSeconds": 0.001989825
    }
  },
  "payloadSize": 1024,
  "topology": "routemesh",
  "diagnostics": "Off",
  "metrics": {
    "messages.admitted": null,
    "messages.expired": null,
    "messages.duplicateReply": null,
    "messages.lateReply": null,
    "messages.unknownCorrelation": null,
    "messages.published": null,
    "messages.publishedInWindow": null,
    "messages.settlePublished": null,
    "messages.sent": "529557",
    "messages.completed": "529541",
    "messages.settleCompleted": "16",
    "messages.failed": "0",
    "messages.timeout": "0",
    "messages.cancelled": "0",
    "messages.unresolved": "0",
    "latency.meanMs": 0.9008369273502901,
    "latency.p50Ms": 1,
    "latency.p95Ms": 2,
    "latency.p99Ms": 2,
    "latency.maxMs": 9.854816,
    "settle.latency.meanMs": 0.6682885,
    "settle.latency.p50Ms": 1,
    "settle.latency.p95Ms": 2,
    "settle.latency.p99Ms": 2,
    "settle.latency.maxMs": 1.366339,
    "connections.requested": null,
    "connections.connected": null,
    "connections.failed": null,
    "throughput.kops": 17.651366666666664,
    "throughput.messagesPerSec": 35302.03333333334,
    "throughput.megabytesPerSec": 34.47464192708334,
    "errors.byKind": {},
    "errors.harness": {},
    "errors.language": {}
  },
  "processes": [
    {
      "sourceFile": "client-0.json",
      "pid": 97938,
      "resources": {
        "process.cpuPercent": 0.9666643246323409,
        "process.rssMb": 63.28125,
        "process.allocatedMb": 6.884918212890625,
        "gc.gen0": "0",
        "gc.gen1": "0",
        "gc.gen2": "0"
      }
    },
    {
      "sourceFile": "server-channel-1.json",
      "pid": 97829,
      "resources": {
        "process.cpuPercent": 619.773704078899,
        "process.rssMb": 538.8671875,
        "process.allocatedMb": 18279.87924194336,
        "gc.gen0": "52",
        "gc.gen1": "3",
        "gc.gen2": "1"
      }
    },
    {
      "sourceFile": "server-channel-0.json",
      "pid": 97830,
      "resources": {
        "process.cpuPercent": 489.2437063055859,
        "process.rssMb": 1917.65625,
        "process.allocatedMb": 10975.491607666016,
        "gc.gen0": "31",
        "gc.gen1": "7",
        "gc.gen2": "0"
      }
    }
  ],
  "limitations": [
    "Percentiles are nearest-rank bucket upper-bound estimates, not exact observations.",
    "Same-host loopback includes CPU competition. Process CPU is one-core=100%.",
    "Unobservable internal metrics and serialized byte sizes remain null with reasons in result.json."
  ],
  "nullReasons": {}
}
```

### phase1-full-routemesh-4096 · channel-echo-only · 4096 · routemesh

Source: [/dev/shm/zlink-perf-dotnet/perf-results/phase1-full-routemesh-4096/channel-echo-only/4096/request-ordinary-routemesh-sna-nna-37f6e58c68f3085d9e52fd89b0d272f4d86cc01a5bb6aa31296338b481b0145f/summary.json](/dev/shm/zlink-perf-dotnet/perf-results/phase1-full-routemesh-4096/channel-echo-only/4096/request-ordinary-routemesh-sna-nna-37f6e58c68f3085d9e52fd89b0d272f4d86cc01a5bb6aa31296338b481b0145f/summary.json)  
SHA-256: `27cd11c570bafbb1490addc2e56bc989e6825ceba9ef345f6a8ee90f1335d983`

```json
{
  "schemaVersion": 2,
  "runId": "phase1-full-routemesh-4096",
  "cellId": "channel-echo-only/4096/request-ordinary-routemesh-sna-nna-37f6e58c68f3085d9e52fd89b0d272f4d86cc01a5bb6aa31296338b481b0145f",
  "scenario": "channel-echo-only",
  "status": "failed",
  "baselineEligible": false,
  "reasons": [
    {
      "code": "CollectionFailure",
      "message": "RuntimeError: Warmup failed; server-channel-0.json",
      "sourceFile": "logs/"
    },
    {
      "code": "CollectionFailure",
      "message": "PhaseMismatch: original has no completed measured reset epoch",
      "sourceFile": "client-0.json"
    },
    {
      "code": "CollectionFailure",
      "message": "PhaseMismatch: original has no completed measured reset epoch",
      "sourceFile": "server-channel-1.json"
    },
    {
      "code": "CollectionFailure",
      "message": "PhaseMismatch: original has no completed measured reset epoch",
      "sourceFile": "server-channel-0.json"
    },
    {
      "code": "CollectionFailure",
      "message": "Missing primary owner original.",
      "sourceFile": "server-channel-0.json"
    }
  ],
  "metricOwners": [
    "server-channel-0.json"
  ],
  "ownerWindows": {},
  "payloadSize": 4096,
  "topology": "routemesh",
  "diagnostics": "Off",
  "metrics": {
    "messages.admitted": null,
    "messages.expired": null,
    "messages.duplicateReply": null,
    "messages.lateReply": null,
    "messages.unknownCorrelation": null,
    "messages.published": null,
    "messages.publishedInWindow": null,
    "messages.settlePublished": null,
    "messages.sent": null,
    "messages.completed": null,
    "messages.settleCompleted": null,
    "messages.failed": null,
    "messages.timeout": null,
    "messages.cancelled": null,
    "messages.unresolved": null,
    "latency.meanMs": null,
    "latency.p50Ms": null,
    "latency.p95Ms": null,
    "latency.p99Ms": null,
    "latency.maxMs": null,
    "settle.latency.meanMs": null,
    "settle.latency.p50Ms": null,
    "settle.latency.p95Ms": null,
    "settle.latency.p99Ms": null,
    "settle.latency.maxMs": null,
    "connections.requested": null,
    "connections.connected": null,
    "connections.failed": null,
    "throughput.kops": null,
    "throughput.messagesPerSec": null,
    "throughput.megabytesPerSec": null,
    "errors.byKind": {},
    "errors.harness": {},
    "errors.language": {}
  },
  "processes": [],
  "limitations": [
    "Percentiles are nearest-rank bucket upper-bound estimates, not exact observations.",
    "Same-host loopback includes CPU competition. Process CPU is one-core=100%.",
    "Unobservable internal metrics and serialized byte sizes remain null with reasons in result.json."
  ],
  "nullReasons": {
    "/metrics/latency.meanMs": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/latency.p50Ms": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/latency.p95Ms": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/latency.p99Ms": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/latency.maxMs": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/settle.latency.meanMs": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/settle.latency.p50Ms": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/settle.latency.p95Ms": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/settle.latency.p99Ms": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/settle.latency.maxMs": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    }
  }
}
```

### phase1-full-session · session-echo-only · 1024 · STREAM

Source: [/dev/shm/zlink-perf-dotnet/perf-results/phase1-full-session/session-echo-only/1024/request-ordinary-na-sna-nna-3e57f44d260b8fe26c479f2bf07c131f637ff1a87adac7c2dd662bddb2311358/summary.json](/dev/shm/zlink-perf-dotnet/perf-results/phase1-full-session/session-echo-only/1024/request-ordinary-na-sna-nna-3e57f44d260b8fe26c479f2bf07c131f637ff1a87adac7c2dd662bddb2311358/summary.json)  
SHA-256: `7b832720947c3aebe896077f69a1f2f6cc4e8ce96d8b211f6f0d6e9815f9c576`

```json
{
  "schemaVersion": 2,
  "runId": "phase1-full-session",
  "cellId": "session-echo-only/1024/request-ordinary-na-sna-nna-3e57f44d260b8fe26c479f2bf07c131f637ff1a87adac7c2dd662bddb2311358",
  "scenario": "session-echo-only",
  "status": "valid",
  "baselineEligible": true,
  "reasons": [],
  "metricOwners": [
    "client-0.json"
  ],
  "ownerWindows": {
    "client-0.json": {
      "startedAtUnixMs": "1788671885098",
      "endedAtUnixMs": "1788671915096",
      "startTicks": "149821973171198",
      "endTicks": "149851973171198",
      "measuredSeconds": 30,
      "settleSeconds": 0.047554094
    }
  },
  "payloadSize": 1024,
  "topology": null,
  "diagnostics": "Off",
  "metrics": {
    "messages.admitted": null,
    "messages.expired": null,
    "messages.duplicateReply": null,
    "messages.lateReply": null,
    "messages.unknownCorrelation": null,
    "messages.published": null,
    "messages.publishedInWindow": null,
    "messages.settlePublished": null,
    "messages.sent": "43909",
    "messages.completed": "43845",
    "messages.settleCompleted": "64",
    "messages.failed": "0",
    "messages.timeout": "0",
    "messages.cancelled": "0",
    "messages.unresolved": "0",
    "latency.meanMs": 43.75243329278139,
    "latency.p50Ms": 64,
    "latency.p95Ms": 64,
    "latency.p99Ms": 64,
    "latency.maxMs": 91.136038,
    "settle.latency.meanMs": 44.19318190625,
    "settle.latency.p50Ms": 64,
    "settle.latency.p95Ms": 64,
    "settle.latency.p99Ms": 64,
    "settle.latency.maxMs": 48.135433,
    "connections.requested": "64",
    "connections.connected": "64",
    "connections.failed": "0",
    "throughput.kops": 1.4615,
    "throughput.messagesPerSec": 2924.866666666667,
    "throughput.megabytesPerSec": 2.856315104166667,
    "errors.byKind": {},
    "errors.harness": {},
    "errors.language": {}
  },
  "processes": [
    {
      "sourceFile": "client-0.json",
      "pid": 95730,
      "resources": {
        "process.cpuPercent": 10.633180854422866,
        "process.rssMb": 86.66015625,
        "process.allocatedMb": 507.6659469604492,
        "gc.gen0": "36",
        "gc.gen1": "3",
        "gc.gen2": "1"
      }
    },
    {
      "sourceFile": "server-session-0.json",
      "pid": 95677,
      "resources": {
        "process.cpuPercent": 27.03300409477206,
        "process.rssMb": 495.3125,
        "process.allocatedMb": 844.2977294921875,
        "gc.gen0": "2",
        "gc.gen1": "1",
        "gc.gen2": "0"
      }
    }
  ],
  "limitations": [
    "Percentiles are nearest-rank bucket upper-bound estimates, not exact observations.",
    "Same-host loopback includes CPU competition. Process CPU is one-core=100%.",
    "Unobservable internal metrics and serialized byte sizes remain null with reasons in result.json."
  ],
  "nullReasons": {}
}
```

### phase1-full-session · session-echo-only · 4096 · STREAM

Source: [/dev/shm/zlink-perf-dotnet/perf-results/phase1-full-session/session-echo-only/4096/request-ordinary-na-sna-nna-4ddf747b0947f7ab29be7a8a45db53bd181c2565c19eff4460dc9ca6b7e91bf2/summary.json](/dev/shm/zlink-perf-dotnet/perf-results/phase1-full-session/session-echo-only/4096/request-ordinary-na-sna-nna-4ddf747b0947f7ab29be7a8a45db53bd181c2565c19eff4460dc9ca6b7e91bf2/summary.json)  
SHA-256: `6b5df04b3f2ca9d5559d2c8485e59ef20ff90ac142e0bdf6047795ab812f6bce`

```json
{
  "schemaVersion": 2,
  "runId": "phase1-full-session",
  "cellId": "session-echo-only/4096/request-ordinary-na-sna-nna-4ddf747b0947f7ab29be7a8a45db53bd181c2565c19eff4460dc9ca6b7e91bf2",
  "scenario": "session-echo-only",
  "status": "valid",
  "baselineEligible": true,
  "reasons": [],
  "metricOwners": [
    "client-0.json"
  ],
  "ownerWindows": {
    "client-0.json": {
      "startedAtUnixMs": "1788671926232",
      "endedAtUnixMs": "1788671956231",
      "startTicks": "149863112284518",
      "endTicks": "149893112284518",
      "measuredSeconds": 30,
      "settleSeconds": 0.040308882
    }
  },
  "payloadSize": 4096,
  "topology": null,
  "diagnostics": "Off",
  "metrics": {
    "messages.admitted": null,
    "messages.expired": null,
    "messages.duplicateReply": null,
    "messages.lateReply": null,
    "messages.unknownCorrelation": null,
    "messages.published": null,
    "messages.publishedInWindow": null,
    "messages.settlePublished": null,
    "messages.sent": "43858",
    "messages.completed": "43794",
    "messages.settleCompleted": "64",
    "messages.failed": "0",
    "messages.timeout": "0",
    "messages.cancelled": "0",
    "messages.unresolved": "0",
    "latency.meanMs": 43.80719912035895,
    "latency.p50Ms": 64,
    "latency.p95Ms": 64,
    "latency.p99Ms": 64,
    "latency.maxMs": 94.6198,
    "settle.latency.meanMs": 44.17725934375,
    "settle.latency.p50Ms": 64,
    "settle.latency.p95Ms": 64,
    "settle.latency.p99Ms": 64,
    "settle.latency.maxMs": 45.06336,
    "connections.requested": "64",
    "connections.connected": "64",
    "connections.failed": "0",
    "throughput.kops": 1.4598,
    "throughput.messagesPerSec": 2921.7333333333336,
    "throughput.megabytesPerSec": 11.413020833333334,
    "errors.byKind": {},
    "errors.harness": {},
    "errors.language": {}
  },
  "processes": [
    {
      "sourceFile": "client-0.json",
      "pid": 95926,
      "resources": {
        "process.cpuPercent": 15.232499894717844,
        "process.rssMb": 91.18359375,
        "process.allocatedMb": 1216.5831680297852,
        "gc.gen0": "97",
        "gc.gen1": "49",
        "gc.gen2": "0"
      }
    },
    {
      "sourceFile": "server-session-0.json",
      "pid": 95871,
      "resources": {
        "process.cpuPercent": 35.06642821846142,
        "process.rssMb": 527.1875,
        "process.allocatedMb": 1739.3357238769531,
        "gc.gen0": "5",
        "gc.gen1": "1",
        "gc.gen2": "0"
      }
    }
  ],
  "limitations": [
    "Percentiles are nearest-rank bucket upper-bound estimates, not exact observations.",
    "Same-host loopback includes CPU competition. Process CPU is one-core=100%.",
    "Unobservable internal metrics and serialized byte sizes remain null with reasons in result.json."
  ],
  "nullReasons": {}
}
```

### phase1-smoke · channel-echo-only · 1024 · clientserver

Source: [/dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke/channel-echo-only/1024/request-ordinary-clientserver-sna-nna-ba353c289905445f4c92fe178a1ffdc29577b30190ce9500ba72e24cbe166b53/summary.json](/dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke/channel-echo-only/1024/request-ordinary-clientserver-sna-nna-ba353c289905445f4c92fe178a1ffdc29577b30190ce9500ba72e24cbe166b53/summary.json)  
SHA-256: `65939d6f54a51872d099038ac67c5d7f98857cdf5a0da755118725c546b1ac54`

```json
{
  "schemaVersion": 2,
  "runId": "phase1-smoke",
  "cellId": "channel-echo-only/1024/request-ordinary-clientserver-sna-nna-ba353c289905445f4c92fe178a1ffdc29577b30190ce9500ba72e24cbe166b53",
  "scenario": "channel-echo-only",
  "status": "unsupported",
  "baselineEligible": false,
  "reasons": [
    {
      "code": "PublicContractMismatch",
      "message": "UnsupportedCellError: ClientServer Server reports Degraded/IsReady=false despite Serving, a Ready target and a typed probe reply. Required public status: framework/doc/framework/common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.ko.md:359; readiness meaning: framework/doc/framework/common/spec/server/06-observability/01-runtime-monitoring.ko.md:194. Runtime implementation gates Selectable on HasClient at framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkClientServerRuntimeService.cs:99.",
      "sourceFile": "framework/doc/framework/common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.ko.md:359"
    },
    {
      "code": "CollectionFailure",
      "message": "[Errno 2] No such file or directory: '/dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke/channel-echo-only/1024/request-ordinary-clientserver-sna-nna-ba353c289905445f4c92fe178a1ffdc29577b30190ce9500ba72e24cbe166b53/client-0.json'",
      "sourceFile": "client-0.json"
    },
    {
      "code": "CollectionFailure",
      "message": "PhaseMismatch: original has no completed measured reset epoch",
      "sourceFile": "server-channel-1.json"
    },
    {
      "code": "CollectionFailure",
      "message": "PhaseMismatch: original has no completed measured reset epoch",
      "sourceFile": "server-channel-0.json"
    },
    {
      "code": "CollectionFailure",
      "message": "Missing primary owner original.",
      "sourceFile": "server-channel-0.json"
    }
  ],
  "metricOwners": [
    "server-channel-0.json"
  ],
  "ownerWindows": {},
  "payloadSize": 1024,
  "topology": "clientserver",
  "metrics": {
    "messages.admitted": null,
    "messages.expired": null,
    "messages.duplicateReply": null,
    "messages.lateReply": null,
    "messages.unknownCorrelation": null,
    "messages.published": null,
    "messages.publishedInWindow": null,
    "messages.settlePublished": null,
    "messages.sent": null,
    "messages.completed": null,
    "messages.settleCompleted": null,
    "messages.failed": null,
    "messages.timeout": null,
    "messages.cancelled": null,
    "messages.unresolved": null,
    "latency.meanMs": null,
    "latency.p50Ms": null,
    "latency.p95Ms": null,
    "latency.p99Ms": null,
    "latency.maxMs": null,
    "settle.latency.meanMs": null,
    "settle.latency.p50Ms": null,
    "settle.latency.p95Ms": null,
    "settle.latency.p99Ms": null,
    "settle.latency.maxMs": null,
    "connections.requested": null,
    "connections.connected": null,
    "connections.failed": null,
    "throughput.kops": null,
    "throughput.messagesPerSec": null,
    "throughput.megabytesPerSec": null,
    "errors.byKind": {},
    "errors.harness": {},
    "errors.language": {}
  },
  "processes": [],
  "limitations": [
    "Percentiles are nearest-rank bucket upper-bound estimates, not exact observations.",
    "Same-host loopback includes CPU competition. Process CPU is one-core=100%.",
    "Unobservable internal metrics and serialized byte sizes remain null with reasons in result.json."
  ],
  "nullReasons": {
    "/metrics/latency.meanMs": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/latency.p50Ms": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/latency.p95Ms": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/latency.p99Ms": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/latency.maxMs": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/settle.latency.meanMs": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/settle.latency.p50Ms": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/settle.latency.p95Ms": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/settle.latency.p99Ms": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/settle.latency.maxMs": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    }
  }
}
```

### phase1-smoke · channel-echo-only · 1024 · routemesh

Source: [/dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke/channel-echo-only/1024/request-ordinary-routemesh-sna-nna-ac003987c57f49183515e497a6ca4b9343fa0da3d10b1bb544ab132894448d1f/summary.json](/dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke/channel-echo-only/1024/request-ordinary-routemesh-sna-nna-ac003987c57f49183515e497a6ca4b9343fa0da3d10b1bb544ab132894448d1f/summary.json)  
SHA-256: `4fb69331e6d15b17d9f0e1b20c68ec2267942eb1d046e8b45d04096ad94e25e8`

```json
{
  "schemaVersion": 2,
  "runId": "phase1-smoke",
  "cellId": "channel-echo-only/1024/request-ordinary-routemesh-sna-nna-ac003987c57f49183515e497a6ca4b9343fa0da3d10b1bb544ab132894448d1f",
  "scenario": "channel-echo-only",
  "status": "valid",
  "baselineEligible": true,
  "reasons": [],
  "metricOwners": [
    "server-channel-0.json"
  ],
  "ownerWindows": {
    "server-channel-0.json": {
      "startedAtUnixMs": "1788671351909",
      "endedAtUnixMs": "1788671353910",
      "startTicks": "149288720425910",
      "endTicks": "149290720425910",
      "measuredSeconds": 2,
      "settleSeconds": 0.001324023
    }
  },
  "payloadSize": 1024,
  "topology": "routemesh",
  "metrics": {
    "messages.admitted": null,
    "messages.expired": null,
    "messages.duplicateReply": null,
    "messages.lateReply": null,
    "messages.unknownCorrelation": null,
    "messages.published": null,
    "messages.publishedInWindow": null,
    "messages.settlePublished": null,
    "messages.sent": "22957",
    "messages.completed": "22941",
    "messages.settleCompleted": "16",
    "messages.failed": "0",
    "messages.timeout": "0",
    "messages.cancelled": "0",
    "messages.unresolved": "0",
    "latency.meanMs": 1.3816455329323045,
    "latency.p50Ms": 2,
    "latency.p95Ms": 4,
    "latency.p99Ms": 4,
    "latency.maxMs": 61.22297,
    "settle.latency.meanMs": 2.3842818125,
    "settle.latency.p50Ms": 4,
    "settle.latency.p95Ms": 4,
    "settle.latency.p99Ms": 4,
    "settle.latency.maxMs": 2.907106,
    "connections.requested": null,
    "connections.connected": null,
    "connections.failed": null,
    "throughput.kops": 11.4705,
    "throughput.messagesPerSec": 22942.5,
    "throughput.megabytesPerSec": 22.40478515625,
    "errors.byKind": {},
    "errors.harness": {},
    "errors.language": {}
  },
  "processes": [
    {
      "sourceFile": "client-0.json",
      "pid": 91205,
      "resources": {
        "process.cpuPercent": 8.48534347410809,
        "process.rssMb": 56.25,
        "process.allocatedMb": 0.53631591796875,
        "gc.gen0": "0",
        "gc.gen1": "0",
        "gc.gen2": "0"
      }
    },
    {
      "sourceFile": "server-channel-1.json",
      "pid": 91099,
      "resources": {
        "process.cpuPercent": 688.958911650598,
        "process.rssMb": 505.4765625,
        "process.allocatedMb": 790.4944381713867,
        "gc.gen0": "4",
        "gc.gen1": "2",
        "gc.gen2": "1"
      }
    },
    {
      "sourceFile": "server-channel-0.json",
      "pid": 91100,
      "resources": {
        "process.cpuPercent": 485.90311674935765,
        "process.rssMb": 552.26171875,
        "process.allocatedMb": 478.0312271118164,
        "gc.gen0": "1",
        "gc.gen1": "0",
        "gc.gen2": "0"
      }
    }
  ],
  "limitations": [
    "Percentiles are nearest-rank bucket upper-bound estimates, not exact observations.",
    "Same-host loopback includes CPU competition. Process CPU is one-core=100%.",
    "Unobservable internal metrics and serialized byte sizes remain null with reasons in result.json."
  ],
  "nullReasons": {}
}
```

### phase1-smoke · channel-echo-only · 4096 · clientserver

Source: [/dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke/channel-echo-only/4096/request-ordinary-clientserver-sna-nna-b1c7260b97162a1373f45944bff2f3496d4afd975c51193d8287e217caafc1e1/summary.json](/dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke/channel-echo-only/4096/request-ordinary-clientserver-sna-nna-b1c7260b97162a1373f45944bff2f3496d4afd975c51193d8287e217caafc1e1/summary.json)  
SHA-256: `021ed99e9121f6e9e3574c926c5f11490d431583a183db831942e394f6a9fd61`

```json
{
  "schemaVersion": 2,
  "runId": "phase1-smoke",
  "cellId": "channel-echo-only/4096/request-ordinary-clientserver-sna-nna-b1c7260b97162a1373f45944bff2f3496d4afd975c51193d8287e217caafc1e1",
  "scenario": "channel-echo-only",
  "status": "unsupported",
  "baselineEligible": false,
  "reasons": [
    {
      "code": "PublicContractMismatch",
      "message": "UnsupportedCellError: ClientServer Server reports Degraded/IsReady=false despite Serving, a Ready target and a typed probe reply. Required public status: framework/doc/framework/common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.ko.md:359; readiness meaning: framework/doc/framework/common/spec/server/06-observability/01-runtime-monitoring.ko.md:194. Runtime implementation gates Selectable on HasClient at framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkClientServerRuntimeService.cs:99.",
      "sourceFile": "framework/doc/framework/common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.ko.md:359"
    },
    {
      "code": "CollectionFailure",
      "message": "[Errno 2] No such file or directory: '/dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke/channel-echo-only/4096/request-ordinary-clientserver-sna-nna-b1c7260b97162a1373f45944bff2f3496d4afd975c51193d8287e217caafc1e1/client-0.json'",
      "sourceFile": "client-0.json"
    },
    {
      "code": "CollectionFailure",
      "message": "PhaseMismatch: original has no completed measured reset epoch",
      "sourceFile": "server-channel-1.json"
    },
    {
      "code": "CollectionFailure",
      "message": "PhaseMismatch: original has no completed measured reset epoch",
      "sourceFile": "server-channel-0.json"
    },
    {
      "code": "CollectionFailure",
      "message": "Missing primary owner original.",
      "sourceFile": "server-channel-0.json"
    }
  ],
  "metricOwners": [
    "server-channel-0.json"
  ],
  "ownerWindows": {},
  "payloadSize": 4096,
  "topology": "clientserver",
  "metrics": {
    "messages.admitted": null,
    "messages.expired": null,
    "messages.duplicateReply": null,
    "messages.lateReply": null,
    "messages.unknownCorrelation": null,
    "messages.published": null,
    "messages.publishedInWindow": null,
    "messages.settlePublished": null,
    "messages.sent": null,
    "messages.completed": null,
    "messages.settleCompleted": null,
    "messages.failed": null,
    "messages.timeout": null,
    "messages.cancelled": null,
    "messages.unresolved": null,
    "latency.meanMs": null,
    "latency.p50Ms": null,
    "latency.p95Ms": null,
    "latency.p99Ms": null,
    "latency.maxMs": null,
    "settle.latency.meanMs": null,
    "settle.latency.p50Ms": null,
    "settle.latency.p95Ms": null,
    "settle.latency.p99Ms": null,
    "settle.latency.maxMs": null,
    "connections.requested": null,
    "connections.connected": null,
    "connections.failed": null,
    "throughput.kops": null,
    "throughput.messagesPerSec": null,
    "throughput.megabytesPerSec": null,
    "errors.byKind": {},
    "errors.harness": {},
    "errors.language": {}
  },
  "processes": [],
  "limitations": [
    "Percentiles are nearest-rank bucket upper-bound estimates, not exact observations.",
    "Same-host loopback includes CPU competition. Process CPU is one-core=100%.",
    "Unobservable internal metrics and serialized byte sizes remain null with reasons in result.json."
  ],
  "nullReasons": {
    "/metrics/latency.meanMs": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/latency.p50Ms": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/latency.p95Ms": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/latency.p99Ms": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/latency.maxMs": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/settle.latency.meanMs": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/settle.latency.p50Ms": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/settle.latency.p95Ms": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/settle.latency.p99Ms": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    },
    "/metrics/settle.latency.maxMs": {
      "code": "PHASE_NOT_STARTED",
      "reason": "No completed measured owner window is available."
    }
  }
}
```

### phase1-smoke · channel-echo-only · 4096 · routemesh

Source: [/dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke/channel-echo-only/4096/request-ordinary-routemesh-sna-nna-00fbf9142615e83fad986a975fc95ed0c5433c6703a2947320d6300fca737b3c/summary.json](/dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke/channel-echo-only/4096/request-ordinary-routemesh-sna-nna-00fbf9142615e83fad986a975fc95ed0c5433c6703a2947320d6300fca737b3c/summary.json)  
SHA-256: `17f9e84fb27955037a05aba863495b7b7290b356575cb930126eddc7ccc4491b`

```json
{
  "schemaVersion": 2,
  "runId": "phase1-smoke",
  "cellId": "channel-echo-only/4096/request-ordinary-routemesh-sna-nna-00fbf9142615e83fad986a975fc95ed0c5433c6703a2947320d6300fca737b3c",
  "scenario": "channel-echo-only",
  "status": "failed",
  "baselineEligible": false,
  "reasons": [
    {
      "code": "PublicOrApplicationFailure",
      "message": "See original error namespaces and firstErrors evidence.",
      "sourceFile": "server-channel-0.json"
    },
    {
      "code": "EchoOutcomeFailure",
      "message": "timeout=16",
      "sourceFile": "server-channel-0.json"
    }
  ],
  "metricOwners": [
    "server-channel-0.json"
  ],
  "ownerWindows": {
    "server-channel-0.json": {
      "startedAtUnixMs": "1788671386497",
      "endedAtUnixMs": "1788671388499",
      "startTicks": "149323316298665",
      "endTicks": "149325316298665",
      "measuredSeconds": 2,
      "settleSeconds": 0.268096002
    }
  },
  "payloadSize": 4096,
  "topology": "routemesh",
  "metrics": {
    "messages.admitted": null,
    "messages.expired": null,
    "messages.duplicateReply": null,
    "messages.lateReply": null,
    "messages.unknownCorrelation": null,
    "messages.published": null,
    "messages.publishedInWindow": null,
    "messages.settlePublished": null,
    "messages.sent": "11904",
    "messages.completed": "11888",
    "messages.settleCompleted": "0",
    "messages.failed": "0",
    "messages.timeout": "16",
    "messages.cancelled": "0",
    "messages.unresolved": "0",
    "latency.meanMs": 1.6864295865578736,
    "latency.p50Ms": 2,
    "latency.p95Ms": 4,
    "latency.p99Ms": 4,
    "latency.maxMs": 22.330498,
    "settle.latency.meanMs": null,
    "settle.latency.p50Ms": null,
    "settle.latency.p95Ms": null,
    "settle.latency.p99Ms": null,
    "settle.latency.maxMs": null,
    "connections.requested": null,
    "connections.connected": null,
    "connections.failed": null,
    "throughput.kops": 5.944,
    "throughput.messagesPerSec": 11896,
    "throughput.megabytesPerSec": 46.46875,
    "errors.byKind": {
      "DeadlineExceeded": "16"
    },
    "errors.harness": {},
    "errors.language": {}
  },
  "processes": [
    {
      "sourceFile": "client-0.json",
      "pid": 91924,
      "resources": {
        "process.cpuPercent": 8.999330643285749,
        "process.rssMb": 56.09375,
        "process.allocatedMb": 0.5373687744140625,
        "gc.gen0": "0",
        "gc.gen1": "0",
        "gc.gen2": "0"
      }
    },
    {
      "sourceFile": "server-channel-1.json",
      "pid": 91811,
      "resources": {
        "process.cpuPercent": 486.8474587634067,
        "process.rssMb": 514.3046875,
        "process.allocatedMb": 703.541877746582,
        "gc.gen0": "2",
        "gc.gen1": "1",
        "gc.gen2": "0"
      }
    },
    {
      "sourceFile": "server-channel-0.json",
      "pid": 91812,
      "resources": {
        "process.cpuPercent": 355.7335100670705,
        "process.rssMb": 622.921875,
        "process.allocatedMb": 541.8745498657227,
        "gc.gen0": "1",
        "gc.gen1": "1",
        "gc.gen2": "0"
      }
    }
  ],
  "limitations": [
    "Percentiles are nearest-rank bucket upper-bound estimates, not exact observations.",
    "Same-host loopback includes CPU competition. Process CPU is one-core=100%.",
    "Unobservable internal metrics and serialized byte sizes remain null with reasons in result.json."
  ],
  "nullReasons": {
    "/metrics/settle.latency.meanMs": {
      "code": "NO_SAMPLES",
      "reason": "No successful samples.",
      "owner": "perf/README.ko.md §15.3"
    },
    "/metrics/settle.latency.p50Ms": {
      "code": "NO_SAMPLES",
      "reason": "No successful samples.",
      "owner": "perf/README.ko.md §15.3"
    },
    "/metrics/settle.latency.p95Ms": {
      "code": "NO_SAMPLES",
      "reason": "No successful samples.",
      "owner": "perf/README.ko.md §15.3"
    },
    "/metrics/settle.latency.p99Ms": {
      "code": "NO_SAMPLES",
      "reason": "No successful samples.",
      "owner": "perf/README.ko.md §15.3"
    },
    "/metrics/settle.latency.maxMs": {
      "code": "NO_SAMPLES",
      "reason": "No successful samples.",
      "owner": "perf/README.ko.md §15.3"
    }
  }
}
```

### phase1-smoke · session-echo-only · 1024 · STREAM

Source: [/dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke/session-echo-only/1024/request-ordinary-na-sna-nna-4f2a07303a0ba8a627135ba09d48ddbe6d53f94ab7bac0ea3ec19d03dc02a7b6/summary.json](/dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke/session-echo-only/1024/request-ordinary-na-sna-nna-4f2a07303a0ba8a627135ba09d48ddbe6d53f94ab7bac0ea3ec19d03dc02a7b6/summary.json)  
SHA-256: `9e67797e6f4076dbcb4e3d9c1fb0c7286a74f836a4cda3bf32bdfa68eb9ba0c9`

```json
{
  "schemaVersion": 2,
  "runId": "phase1-smoke",
  "cellId": "session-echo-only/1024/request-ordinary-na-sna-nna-4f2a07303a0ba8a627135ba09d48ddbe6d53f94ab7bac0ea3ec19d03dc02a7b6",
  "scenario": "session-echo-only",
  "status": "valid",
  "baselineEligible": true,
  "reasons": [],
  "metricOwners": [
    "client-0.json"
  ],
  "ownerWindows": {
    "client-0.json": {
      "startedAtUnixMs": "1788671338283",
      "endedAtUnixMs": "1788671340285",
      "startTicks": "149275091971759",
      "endTicks": "149277091971759",
      "measuredSeconds": 2,
      "settleSeconds": 0.036896113
    }
  },
  "payloadSize": 1024,
  "topology": null,
  "metrics": {
    "messages.admitted": null,
    "messages.expired": null,
    "messages.duplicateReply": null,
    "messages.lateReply": null,
    "messages.unknownCorrelation": null,
    "messages.published": null,
    "messages.publishedInWindow": null,
    "messages.settlePublished": null,
    "messages.sent": "2940",
    "messages.completed": "2876",
    "messages.settleCompleted": "64",
    "messages.failed": "0",
    "messages.timeout": "0",
    "messages.cancelled": "0",
    "messages.unresolved": "0",
    "latency.meanMs": 43.86346773713491,
    "latency.p50Ms": 64,
    "latency.p95Ms": 64,
    "latency.p99Ms": 64,
    "latency.maxMs": 50.07503,
    "settle.latency.meanMs": 46.383702140625,
    "settle.latency.p50Ms": 64,
    "settle.latency.p95Ms": 64,
    "settle.latency.p99Ms": 64,
    "settle.latency.maxMs": 48.62276,
    "connections.requested": "64",
    "connections.connected": "64",
    "connections.failed": "0",
    "throughput.kops": 1.438,
    "throughput.messagesPerSec": 2908,
    "throughput.megabytesPerSec": 2.83984375,
    "errors.byKind": {},
    "errors.harness": {},
    "errors.language": {}
  },
  "processes": [
    {
      "sourceFile": "client-0.json",
      "pid": 90600,
      "resources": {
        "process.cpuPercent": 35.461633544474545,
        "process.rssMb": 84.53125,
        "process.allocatedMb": 33.943695068359375,
        "gc.gen0": "3",
        "gc.gen1": "1",
        "gc.gen2": "0"
      }
    },
    {
      "sourceFile": "server-session-0.json",
      "pid": 90547,
      "resources": {
        "process.cpuPercent": 66.99045677399457,
        "process.rssMb": 225.46875,
        "process.allocatedMb": 56.11272430419922,
        "gc.gen0": "0",
        "gc.gen1": "0",
        "gc.gen2": "0"
      }
    }
  ],
  "limitations": [
    "Percentiles are nearest-rank bucket upper-bound estimates, not exact observations.",
    "Same-host loopback includes CPU competition. Process CPU is one-core=100%.",
    "Unobservable internal metrics and serialized byte sizes remain null with reasons in result.json."
  ],
  "nullReasons": {}
}
```

### phase1-smoke · session-echo-only · 4096 · STREAM

Source: [/dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke/session-echo-only/4096/request-ordinary-na-sna-nna-7797f480caf42b0ed48385b13d937dddfb7c383a55e239af51d1a524b1edee56/summary.json](/dev/shm/zlink-perf-dotnet/perf-results/phase1-smoke/session-echo-only/4096/request-ordinary-na-sna-nna-7797f480caf42b0ed48385b13d937dddfb7c383a55e239af51d1a524b1edee56/summary.json)  
SHA-256: `edd1e762bf39ed4c9fe18e0c15a417eeb8ba7aad3a5a6d02d3b15ef6dcc14d29`

```json
{
  "schemaVersion": 2,
  "runId": "phase1-smoke",
  "cellId": "session-echo-only/4096/request-ordinary-na-sna-nna-7797f480caf42b0ed48385b13d937dddfb7c383a55e239af51d1a524b1edee56",
  "scenario": "session-echo-only",
  "status": "valid",
  "baselineEligible": true,
  "reasons": [],
  "metricOwners": [
    "client-0.json"
  ],
  "ownerWindows": {
    "client-0.json": {
      "startedAtUnixMs": "1788671342678",
      "endedAtUnixMs": "1788671344679",
      "startTicks": "149279487552634",
      "endTicks": "149281487552634",
      "measuredSeconds": 2,
      "settleSeconds": 0.037654734
    }
  },
  "payloadSize": 4096,
  "topology": null,
  "metrics": {
    "messages.admitted": null,
    "messages.expired": null,
    "messages.duplicateReply": null,
    "messages.lateReply": null,
    "messages.unknownCorrelation": null,
    "messages.published": null,
    "messages.publishedInWindow": null,
    "messages.settlePublished": null,
    "messages.sent": "2924",
    "messages.completed": "2860",
    "messages.settleCompleted": "64",
    "messages.failed": "0",
    "messages.timeout": "0",
    "messages.cancelled": "0",
    "messages.unresolved": "0",
    "latency.meanMs": 44.27829526013986,
    "latency.p50Ms": 64,
    "latency.p95Ms": 64,
    "latency.p99Ms": 64,
    "latency.maxMs": 50.667628,
    "settle.latency.meanMs": 45.943537703125,
    "settle.latency.p50Ms": 64,
    "settle.latency.p95Ms": 64,
    "settle.latency.p99Ms": 64,
    "settle.latency.maxMs": 48.24579,
    "connections.requested": "64",
    "connections.connected": "64",
    "connections.failed": "0",
    "throughput.kops": 1.43,
    "throughput.messagesPerSec": 2892,
    "throughput.megabytesPerSec": 11.296875,
    "errors.byKind": {},
    "errors.harness": {},
    "errors.language": {}
  },
  "processes": [
    {
      "sourceFile": "client-0.json",
      "pid": 90730,
      "resources": {
        "process.cpuPercent": 40.984338961935016,
        "process.rssMb": 94.53125,
        "process.allocatedMb": 80.44902801513672,
        "gc.gen0": "7",
        "gc.gen1": "4",
        "gc.gen2": "1"
      }
    },
    {
      "sourceFile": "server-session-0.json",
      "pid": 90677,
      "resources": {
        "process.cpuPercent": 78.36220957547,
        "process.rssMb": 320.3125,
        "process.allocatedMb": 114.30387878417969,
        "gc.gen0": "0",
        "gc.gen1": "0",
        "gc.gen2": "0"
      }
    }
  ],
  "limitations": [
    "Percentiles are nearest-rank bucket upper-bound estimates, not exact observations.",
    "Same-host loopback includes CPU competition. Process CPU is one-core=100%.",
    "Unobservable internal metrics and serialized byte sizes remain null with reasons in result.json."
  ],
  "nullReasons": {}
}
```

## collect_env.sh 출력 사본

Source: [/dev/shm/zlink-perf-dotnet/collect-env-report.json](/dev/shm/zlink-perf-dotnet/collect-env-report.json)  
SHA-256: `5733f44977cc5b54d383879fc8e7c5269fec1b652f58a8bc35252cdaa83db2a5`

```json
{
  "schemaVersion": 2,
  "commit": "79094d9f1f87bf561d08f9b38c17d1e0e69b77f4",
  "dirty": true,
  "buildMode": "Release",
  "frameworkVersion": "0.10.0",
  "bindingVersion": "0.17.0",
  "coreVersion": "0.17.0",
  "cpuModel": "Intel(R) Core(TM) Ultra 7 265K",
  "effectiveProcessorCount": 20,
  "cpuAffinity": [
    0,
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    9,
    10,
    11,
    12,
    13,
    14,
    15,
    16,
    17,
    18,
    19
  ],
  "loadAverage": [
    1.1005859375,
    6.3818359375,
    7.48974609375
  ],
  "cpuQuota": null,
  "cpuset": "0-19",
  "memoryLimit": null,
  "memoryCurrent": null,
  "memoryAvailable": "MemAvailable:   88783544 kB",
  "os": "Linux-6.6.87.2-microsoft-standard-WSL2-x86_64-with-glibc2.39",
  "kernel": "6.6.87.2-microsoft-standard-WSL2",
  "host": "ulalax-home",
  "container": false,
  "cgroup": "0::/init.scope",
  "fdLimit": {
    "soft": "1048576",
    "hard": "1048576"
  },
  "ephemeralPortRange": "1024\t65535",
  "listenBacklog": "65535",
  "tcpMaxSynBacklog": "65535",
  "tcpTimeWaitReuse": "1",
  "dotnetInfo": ".NET SDK:\n Version:           8.0.130\n Commit:            8e123d44e0\n Workload version:  8.0.100-manifests.f8c82334\n\nRuntime Environment:\n OS Name:     ubuntu\n OS Version:  24.04\n OS Platform: Linux\n RID:         ubuntu.24.04-x64\n Base Path:   /usr/lib/dotnet/sdk/8.0.130/\n\n.NET workloads installed:\n Workload version: 8.0.100-manifests.f8c82334\nThere are no installed workloads to display.\n\nHost:\n  Version:      8.0.30\n  Architecture: x64\n  Commit:       a83db3e0eb\n\n.NET SDKs installed:\n  8.0.130 [/usr/lib/dotnet/sdk]\n\n.NET runtimes installed:\n  Microsoft.AspNetCore.App 8.0.30 [/usr/lib/dotnet/shared/Microsoft.AspNetCore.App]\n  Microsoft.NETCore.App 8.0.30 [/usr/lib/dotnet/shared/Microsoft.NETCore.App]\n\nOther architectures found:\n  None\n\nEnvironment variables:\n  Not set\n\nglobal.json file:\n  Not found\n\nLearn more:\n  https://aka.ms/dotnet/info\n\nDownload .NET:\n  https://aka.ms/dotnet/download\n",
  "installedRuntimes": "Microsoft.AspNetCore.App 8.0.30 [/usr/lib/dotnet/shared/Microsoft.AspNetCore.App]\nMicrosoft.NETCore.App 8.0.30 [/usr/lib/dotnet/shared/Microsoft.NETCore.App]\n",
  "runtimeSettings": {
    "Client": {
      "runtimeOptions": {
        "tfm": "net8.0",
        "framework": {
          "name": "Microsoft.NETCore.App",
          "version": "8.0.0"
        },
        "configProperties": {
          "System.Reflection.Metadata.MetadataUpdater.IsSupported": false,
          "System.Runtime.Serialization.EnableUnsafeBinaryFormatterSerialization": false
        }
      }
    },
    "SessionServer": {
      "runtimeOptions": {
        "tfm": "net8.0",
        "frameworks": [
          {
            "name": "Microsoft.NETCore.App",
            "version": "8.0.0"
          },
          {
            "name": "Microsoft.AspNetCore.App",
            "version": "8.0.0"
          }
        ],
        "configProperties": {
          "System.GC.Server": true,
          "System.Reflection.Metadata.MetadataUpdater.IsSupported": false,
          "System.Runtime.Serialization.EnableUnsafeBinaryFormatterSerialization": false
        }
      }
    },
    "ChannelServer": {
      "runtimeOptions": {
        "tfm": "net8.0",
        "frameworks": [
          {
            "name": "Microsoft.NETCore.App",
            "version": "8.0.0"
          },
          {
            "name": "Microsoft.AspNetCore.App",
            "version": "8.0.0"
          }
        ],
        "configProperties": {
          "System.GC.Server": true,
          "System.Reflection.Metadata.MetadataUpdater.IsSupported": false,
          "System.Runtime.Serialization.EnableUnsafeBinaryFormatterSerialization": false
        }
      }
    }
  },
  "runtimeOptions": {
    "DOTNET_PROCESSOR_COUNT": null,
    "DOTNET_GCHeapHardLimit": null,
    "DOTNET_gcServer": null,
    "DOTNET_ThreadPool_ForceMinWorkerThreads": null,
    "DOTNET_ThreadPool_ForceMaxWorkerThreads": null
  },
  "environment": {
    "TMPDIR": "/dev/shm/zlink-tmp-dotnet",
    "ZLINK_LIBRARY_PATH": "/home/hep7/project/zlink/core/build-dev/lib",
    "NUGET_PACKAGES": "/dev/shm/zlink-tmp-dotnet/nuget-350b8b789a1b3132",
    "UseSharedCompilation": "false",
    "MSBUILDDISABLENODEREUSE": "1",
    "DOTNET_CLI_TELEMETRY_OPTOUT": "1"
  },
  "serializer": {
    "name": "default Framework typed JSON / ZlinkStreamJsonCodec",
    "runtime": "System.Text.Json",
    "options": "lowerCamelCase, canonical decimal-string 64-bit values, Base64 text payload, no compression or custom message codec"
  },
  "clock": {
    "source": "System.Diagnostics.Stopwatch",
    "unit": "ns",
    "scope": "process"
  },
  "deployment": "same-host loopback; source and target share CPU resources",
  "artifacts": [
    {
      "path": "/home/hep7/project/zlink/.artifacts/wsl/nuget/Systems.Zlink.0.17.0.nupkg",
      "resolvedPath": "/home/hep7/project/zlink/.artifacts/wsl/nuget/Systems.Zlink.0.17.0.nupkg",
      "sha256": "350b8b789a1b31328bd477d895283efc6b986a1b242c310eadda914cf79c98c3"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.Shared/histogram-bounds.json",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.Shared/histogram-bounds.json",
      "sha256": "5496ba4c4db91d64d29002ee5de1c21e6fe811a5fc38d9c98b8da3cec8284b78"
    },
    {
      "path": "/home/hep7/project/zlink/core/build-dev/lib/libzlink.so.0",
      "resolvedPath": "/home/hep7/project/zlink/core/build-dev/lib/libzlink.so.0.17.0",
      "sha256": "64567f1715b3f1527afbc1c290e2b262d02d722768e160227a6f9815bdd4bb43"
    },
    {
      "path": "/home/hep7/project/zlink/core/build-dev/lib/libzlink.so",
      "resolvedPath": "/home/hep7/project/zlink/core/build-dev/lib/libzlink.so.0.17.0",
      "sha256": "64567f1715b3f1527afbc1c290e2b262d02d722768e160227a6f9815bdd4bb43"
    },
    {
      "path": "/home/hep7/project/zlink/core/build-dev/lib/libzlink.so.0.17.0",
      "resolvedPath": "/home/hep7/project/zlink/core/build-dev/lib/libzlink.so.0.17.0",
      "sha256": "64567f1715b3f1527afbc1c290e2b262d02d722768e160227a6f9815bdd4bb43"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.Client/bin/Release/net8.0/ZLink.Framework.Perf.Shared.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.Client/bin/Release/net8.0/ZLink.Framework.Perf.Shared.dll",
      "sha256": "2deebc38385cafe2eda81eaa64bd3ef22a241015970676b211a1851ac56e730a"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.Client/bin/Release/net8.0/Systems.Zlink.Stream.Connector.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.Client/bin/Release/net8.0/Systems.Zlink.Stream.Connector.dll",
      "sha256": "124eec09c3a4f43b4043b061a85c6f7dd880f0e2de6aa8785d72914e8ef7a63d"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.Client/bin/Release/net8.0/K4os.Compression.LZ4.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.Client/bin/Release/net8.0/K4os.Compression.LZ4.dll",
      "sha256": "e282aadc2353b864fe95cd5fb525a1a093d99a30a5bf2530b7ab831d56179816"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.Client/bin/Release/net8.0/ZLink.Framework.Perf.Client.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.Client/bin/Release/net8.0/ZLink.Framework.Perf.Client.dll",
      "sha256": "484020e9997a028aa4966815e6756650c232991cec3e6d2d7f5f8db48dc6bb55"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.Client/bin/Release/net8.0/Zlink.Framework.Contracts.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.Client/bin/Release/net8.0/Zlink.Framework.Contracts.dll",
      "sha256": "ab6b633260306288bc0e5841b0682c2883f50ec41a2c807d8414a72130cb4f5a"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.Client/bin/Release/net8.0/ZLink.Framework.Perf.Client.runtimeconfig.json",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.Client/bin/Release/net8.0/ZLink.Framework.Perf.Client.runtimeconfig.json",
      "sha256": "97c9700542b659150b230c3578b29530fd76ab01ec66a92cd16945e0245713df"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/Zlink.Framework.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/Zlink.Framework.dll",
      "sha256": "89f146963bdac1bef73d9cec9431cba324377b5eaf4846999533c398facfbe03"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/ZLink.Framework.Perf.Shared.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/ZLink.Framework.Perf.Shared.dll",
      "sha256": "2deebc38385cafe2eda81eaa64bd3ef22a241015970676b211a1851ac56e730a"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/Zlink.Framework.AspNetCore.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/Zlink.Framework.AspNetCore.dll",
      "sha256": "a213fecf3ffa0010172682169ea8a12b2ea901cca4e9232b4af388fbcea637d8"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/Zlink.HttpClient.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/Zlink.HttpClient.dll",
      "sha256": "7605367a59a520714acde3b755be1c18a477d019832a96ed257026e812fef222"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/Systems.Zlink.Stream.Connector.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/Systems.Zlink.Stream.Connector.dll",
      "sha256": "124eec09c3a4f43b4043b061a85c6f7dd880f0e2de6aa8785d72914e8ef7a63d"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/ZLink.Framework.Perf.SessionServer.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/ZLink.Framework.Perf.SessionServer.dll",
      "sha256": "f154b51378dca479516b1537051d2f685e3c329b436382d72a441ff501dd3800"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/Zlink.Framework.Provider.Abstractions.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/Zlink.Framework.Provider.Abstractions.dll",
      "sha256": "95056f3938b3ddb0c3fea29fab9df143909009590528d40dddff7c2c5378a12b"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/K4os.Compression.LZ4.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/K4os.Compression.LZ4.dll",
      "sha256": "e282aadc2353b864fe95cd5fb525a1a093d99a30a5bf2530b7ab831d56179816"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/Systems.Zlink.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/Systems.Zlink.dll",
      "sha256": "f860aad11ccae2d34f77e2b2cb049c80af78972485d82b77fc8433d93413fdcf"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/Zlink.Framework.Contracts.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/Zlink.Framework.Contracts.dll",
      "sha256": "ab6b633260306288bc0e5841b0682c2883f50ec41a2c807d8414a72130cb4f5a"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/ZLink.Framework.Perf.ServerSupport.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/ZLink.Framework.Perf.ServerSupport.dll",
      "sha256": "7ede9bff7492025abfdc5a7206d212e28b757bb0b60d0dfbdbd1821ba2d6249e"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/ZLink.Framework.Perf.SessionServer.runtimeconfig.json",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/ZLink.Framework.Perf.SessionServer.runtimeconfig.json",
      "sha256": "4351dd23692b878de26903bbf54de76dfa20dd13d0600ee1cfa3b005592bb4cd"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/runtimes/linux-x64/native/libzlink.so",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.SessionServer/bin/Release/net8.0/runtimes/linux-x64/native/libzlink.so",
      "sha256": "64567f1715b3f1527afbc1c290e2b262d02d722768e160227a6f9815bdd4bb43"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/Zlink.Framework.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/Zlink.Framework.dll",
      "sha256": "89f146963bdac1bef73d9cec9431cba324377b5eaf4846999533c398facfbe03"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/ZLink.Framework.Perf.Shared.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/ZLink.Framework.Perf.Shared.dll",
      "sha256": "2deebc38385cafe2eda81eaa64bd3ef22a241015970676b211a1851ac56e730a"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/Zlink.Framework.AspNetCore.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/Zlink.Framework.AspNetCore.dll",
      "sha256": "a213fecf3ffa0010172682169ea8a12b2ea901cca4e9232b4af388fbcea637d8"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/Zlink.HttpClient.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/Zlink.HttpClient.dll",
      "sha256": "7605367a59a520714acde3b755be1c18a477d019832a96ed257026e812fef222"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/Systems.Zlink.Stream.Connector.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/Systems.Zlink.Stream.Connector.dll",
      "sha256": "124eec09c3a4f43b4043b061a85c6f7dd880f0e2de6aa8785d72914e8ef7a63d"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/ZLink.Framework.Perf.ChannelServer.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/ZLink.Framework.Perf.ChannelServer.dll",
      "sha256": "52d37b207750027a225fe26bf3b514f3d0eacb68ae1b0c8b8b9dd3e17e22092d"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/Zlink.Framework.Provider.Abstractions.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/Zlink.Framework.Provider.Abstractions.dll",
      "sha256": "95056f3938b3ddb0c3fea29fab9df143909009590528d40dddff7c2c5378a12b"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/K4os.Compression.LZ4.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/K4os.Compression.LZ4.dll",
      "sha256": "e282aadc2353b864fe95cd5fb525a1a093d99a30a5bf2530b7ab831d56179816"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/Systems.Zlink.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/Systems.Zlink.dll",
      "sha256": "f860aad11ccae2d34f77e2b2cb049c80af78972485d82b77fc8433d93413fdcf"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/Zlink.Framework.Contracts.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/Zlink.Framework.Contracts.dll",
      "sha256": "ab6b633260306288bc0e5841b0682c2883f50ec41a2c807d8414a72130cb4f5a"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/ZLink.Framework.Perf.ServerSupport.dll",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/ZLink.Framework.Perf.ServerSupport.dll",
      "sha256": "7ede9bff7492025abfdc5a7206d212e28b757bb0b60d0dfbdbd1821ba2d6249e"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/ZLink.Framework.Perf.ChannelServer.runtimeconfig.json",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/ZLink.Framework.Perf.ChannelServer.runtimeconfig.json",
      "sha256": "4351dd23692b878de26903bbf54de76dfa20dd13d0600ee1cfa3b005592bb4cd"
    },
    {
      "path": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/runtimes/linux-x64/native/libzlink.so",
      "resolvedPath": "/home/hep7/project/zlink/framework/languages/dotnet/perf/ZLink.Framework.Perf.ChannelServer/bin/Release/net8.0/runtimes/linux-x64/native/libzlink.so",
      "sha256": "64567f1715b3f1527afbc1c290e2b262d02d722768e160227a6f9815bdd4bb43"
    }
  ]
}
```
