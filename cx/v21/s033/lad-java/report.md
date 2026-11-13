# #4350 Java L0 측정 보고서

## 범위와 유효성

PM 2026-11-12 08:30 결정에 따라 current main을 다시 측정하지 않았다. main 값은 30초 warmup 기준선 `../r4190w/report.md`와 그 raw를 사용했다. `raw/main/**`와 `raw/main-java-driver.log`는 보존했지만 이번 비교에 쓰지 않았다.

요청 결과는 회신 정정 전 `raw/l0/bench/java/run*/zlink-framework-java-request-*/`에 기록된 결과를 무효로 처리했다. 회신 정정 후 `raw/l0-reply/bench/java/`에서 binding과 L0를 같은 측정 그룹으로 다시 실행했다. 기존 파일은 보존했다. send-saturation은 회신 경로에 영향이 없어 기존 `raw/l0/bench/java/`의 L0·binding 결과를 사용했다.

각 유효 결과 파일에서 `errors=0`, `server_errors=0`, `submitted=completed=received`, `abandoned=0`, `drain_bound_hit=false`였다. 확인한 파일은 요청 24개, 교차 9개, send 12개로 총 45개다. 측정은 `flock /root/jobs/fx/cx/measure.lock` exclusive 안에서 30초 warmup, 5초 active, 3회 실행했다. source 0–7·target 8–15. 집계에는 r4190w의 `bench_aggregate.py`를 재사용했다. 재현 명령은 `tools/run-group.sh`다. 이 파일은 r4190w 사본에서 JOB 및 raw 경로만 바꿨다.

## L0·binding 및 main 기준선 비교

처리량 단위는 msg/s이며 세 run 중앙값이다. L0 판정 기준은 L0÷binding ≥ 0.95다. `main`은 기준선 수치를 msg/s로 환산했다. 이 표의 각 행 근거에는 측정 숫자와 비율에 쓰인 raw를 적었다.

| 패턴 | 크기 | 이번 run binding | L0 | L0÷binding | L0 판정 | 기준선 binding | main | main÷기준선 binding | main 0.95 판정 | raw 근거 |
|---|---:|---:|---:|---:|---|---:|---:|---:|---|---|
| send-saturation | 1K | 554,016.4 | 548,098.4 | 0.989 | 통과 | 566,030 | 111,740 | 0.197 | 미달 | `raw/l0/bench/java/run{1,2,3}/{zlink-java,zlink-framework-java}-send-saturation-1024/results.json`; `../r4190w/raw/bench/java/run{1,2,3}/{zlink-java,zlink-framework-java}-send-saturation-1024/results.json` |
| send-saturation | 4K | 199,386.2 | 195,175.8 | 0.979 | 통과 | 204,150 | 104,270 | 0.511 | 미달 | `raw/l0/bench/java/run{1,2,3}/{zlink-java,zlink-framework-java}-send-saturation-4096/results.json`; `../r4190w/raw/bench/java/run{1,2,3}/{zlink-java,zlink-framework-java}-send-saturation-4096/results.json` |
| request-serial | 1K | 15,021.0 | 14,988.2 | 0.998 | 통과 | 15,020 | 9,010 | 0.600 | 미달 | `raw/l0-reply/bench/java/run{1,2,3}/{zlink-java,zlink-framework-java}-request-serial-1024/results.json`; `../r4190w/raw/bench/java/run{1,2,3}/{zlink-java,zlink-framework-java}-request-serial-1024/results.json` |
| request-serial | 4K | 14,020.8 | 13,613.8 | 0.971 | 통과 | 13,900 | 8,450 | 0.608 | 미달 | `raw/l0-reply/bench/java/run{1,2,3}/{zlink-java,zlink-framework-java}-request-serial-4096/results.json`; `../r4190w/raw/bench/java/run{1,2,3}/{zlink-java,zlink-framework-java}-request-serial-4096/results.json` |
| request-backpressure | 1K | 140,543.2 | 141,559.8 | 1.007 | 통과 | 145,510 | 48,930 | 0.336 | 미달 | `raw/l0-reply/bench/java/run{1,2,3}/{zlink-java,zlink-framework-java}-request-backpressure-1024/results.json`; `../r4190w/raw/bench/java/run{1,2,3}/{zlink-java,zlink-framework-java}-request-backpressure-1024/results.json` |
| request-backpressure | 4K | 106,344.2 | 110,859.8 | 1.042 | 통과 | 115,060 | 47,180 | 0.410 | 미달 | `raw/l0-reply/bench/java/run{1,2,3}/{zlink-java,zlink-framework-java}-request-backpressure-4096/results.json`; `../r4190w/raw/bench/java/run{1,2,3}/{zlink-java,zlink-framework-java}-request-backpressure-4096/results.json` |

L0 여섯 셀 모두 기준을 통과했다. Aggregator의 재현성(G5)에서는 request-serial 4K의 run spread가 10%를 넘어 해당 비율이 aggregator상 unsupported다. 위 표는 지시된 3회 중앙값과 0.95 기준을 그대로 보고하고 이 제한도 함께 기록한다. 집계 원본은 `raw/l0-reply/aggregate.json`이다. `raw/l0/aggregate-all.json`에는 회신 정정 전 요청 행이 섞여 있어 send 행만 참고하고 요청 집계에는 사용하지 않았다.

## L0 교차 시험

send-saturation 1K, 3회 중앙값이다. 모든 조합에서 errors 0 및 submitted=completed=received였다. 세 번째 행은 Framework bench client 실행기를 쓰지만 L0 모드에서 client 동작을 `RawStack`으로 대체한다.

| 서버 | 클라이언트 | 처리량 (msg/s) | 기준 조합 대비 | raw 근거 |
|---|---|---:|---:|---|
| raw binding | raw binding | 547,519.8 | 1.000 | `raw/l0-cross/raw-raw/run{1,2,3}/zlink-java-send-saturation-1024/results.json` |
| Framework L0 | raw binding | 558,153.2 | 1.019 | `raw/l0-cross/fw-raw/run{1,2,3}/zlink-java-send-saturation-1024/results.json` |
| Framework L0 | Framework bench client (L0 `RawStack`) | 545,796.6 | 0.997 | `raw/l0-cross/fw-fw/run{1,2,3}/zlink-framework-java-send-saturation-1024/results.json` |

이 L0 cross는 요청 정정 지시에 따라 수행한 단일 send 셀이다. main cross는 수행하지 않았다.

## L0 회신 parity

- binding server: `framework/bench/grpc/java/zlink-raw-server/src/main/java/systems/zlink/bench/withgrpc/rawserver/ZLinkRawBenchServer.java:115-127`은 `Message.from(RESPONSE_ENVELOPE)`와 `RawWire.encodeBenchPayloadMessage(body)`를 만들고 두 파트를 submit한 뒤 wrapper 둘을 닫는다.
- L0 server: `framework/bench/grpc/java/zlink-framework-server/src/main/java/systems/zlink/bench/withgrpc/frameworkserver/ZLinkFrameworkBenchServer.java:158-161`도 같은 header·protobuf body를 생성해 두 파트로 submit하고 wrapper 둘을 닫는다.
- 양쪽 모두 body를 protobuf `ByteString.copyFrom`으로 복사하고 protobuf 인코딩을 한 번 수행한다. 요청 파트를 회신에 재사용하거나 forward하지 않는다. receive decode는 binding `ZLinkRawBenchServer.java:193-198`과 L0 `ZLinkFrameworkBenchServer.java:151` 모두 `RawWire.decodeBenchPayloadBody`를 사용한다.

## L0에서 끈 기능과 대체 경로

L0는 환경 변수 `ZLINK_BENCH_LADDER_L0` 분기만 있는 실험용 bench 코드다. Framework runtime 파일은 변경하지 않았다.

| 사다리 기능 | 끈 항목 및 위치 | L0에서 사용한 대체 |
|---|---|---|
| permit | Framework server 조기 분기 `ZLinkFrameworkBenchServer.java:59-62`; 직접 블로킹 수신 `:118-127`로 Framework application queue permit 경로를 거치지 않는다. | binding `recv(NONE)` 후 `DONT_WAIT`로 비울 때까지 drain |
| envelope header | Framework route envelope 해석 경로를 거치지 않는다: server `:59-62`, client `FrameworkStack.java:57-66,125-130`. | `RawStack.java:69-87` binding request header를 쓰고 `RawWire`의 binding envelope/body 규약 유지 |
| service wire framing | Framework route mesh 송수신을 거치지 않는다: server `:89-93`, client `FrameworkStack.java:57-66`. | binding `RouterSocket` multipart request/reply: `RawStack.java:69-77`, L0 server `:158-160` |
| codec | Framework 기본 codec 등록/사용 `ZLinkFrameworkBenchServer.java:182,196-205`를 조기 분기가 건너뛴다. | `RawWire` protobuf 인코딩·디코딩: L0 server `:151,158-160`, client `RawStack.java:70-72`; binding과 같은 decoder |
| owner queue·worker | Framework mailbox/worker dispatch는 사용하지 않는다. `ZLinkFrameworkBenchServer.java:100-127`의 전용 owner loop가 직접 receive·drain·처리한다. | 단일 owner thread의 블로킹 binding receive와 고정 함수 직접 호출 |
| activation·DI scope | Spring context 진입 `ZLinkFrameworkBenchServer.java:64-67` 및 Framework handler 등록·생성 `:196-215,224-232`를 우회한다. | 고정 처리 동작을 `handleLadderL0`에서 직접 실행. `BenchEchoHandler` 자체는 호출하지 않고 `metrics.record(body)`를 모사로 사용 (`:145-160`). |
| operation registry·completion | Framework route operation 경로 `FrameworkStack.java:86-122`를 우회하고 `:57-66,125-130`에서 binding `RawStack` operation을 쓴다. | client는 binding operation completion, server reply는 binding `received.reply().submit()` (`ZLinkFrameworkBenchServer.java:158-160`) |

모사한 동작은 Framework client/server route-mesh operation과 `BenchEchoHandler`의 `metrics.record` 처리다. L0 측정은 실제 Framework route-mesh runtime 처리량을 뜻하지 않으며 Framework layer를 제거한 binding-shaped bench 상한이다. 이 점은 이후 L1–L7에서 하나씩 되돌릴 때 구분한다.

## L0에 남은 비용

어느 셀도 0.95 아래가 아니므로 지시된 추가 profile/alloc/lock 조사는 필요하지 않았다. 관측된 binding 대비 차이는 send 1K −1.1%, send 4K −2.1%, request-serial 1K −0.2%, request-serial 4K −2.9%이고, request-backpressure는 각각 +0.7%, +4.2% 높았다. 이 작은 차이의 코드 위치별 귀속을 확인하는 profile은 수행하지 않았으며, 측정 오차와 분리된 잔여 비용이라고 단정하지 않는다.

## 빌드와 변경

빌드: `framework/bench/grpc/java/build.sh` 성공. `git diff --check` 통과. 실험 브랜치 로컬 커밋 `5acfffcd0224c565e40d57da2ad2e33de51cb00d`; push하지 않았다. L0 측정 전 built commit `494c5f4827db8d86a5dce716b7a8da6e7e9cf970` + dirty, merge-base 동일 commit으로 기록했다. 실행기와 class JAR은 모두 `/root/worktree/zlink-v21-s033/lad-java/framework/bench/grpc/java/**/build/install/` 아래에서 실행했다. 주요 SHA-256은 `progress.md`의 측정 전 기록을 참조한다. `/root/zlink` 산출물은 사용하지 않았다.

변경 파일:
- `framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/BenchClient.java`
- `framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/FrameworkStack.java`
- `framework/bench/grpc/java/run_local.sh`
- `framework/bench/grpc/java/zlink-framework-server/src/main/java/systems/zlink/bench/withgrpc/frameworkserver/ZLinkFrameworkBenchServer.java`
