# #4351 Java ladder L1 — application job queue permit

## 결과

L1은 6개 셀 모두 단계 손실 7% 이내다. 처리량은 msg/s 중앙값이다. `L1÷binding`은 이번 L1과 binding 측정의 비율이며, 단계 손실은 `1 − (L1÷binding)/(L0÷binding)`으로 계산했다. 음수 손실은 L0보다 처리량이 오른 경우다. 각 행의 raw `results.json`에서 `errors=0`, `server_errors=0`, `submitted=completed=target_stats.received`를 확인했다.

| 셀 | 반복 | binding | L1 | L1÷binding | L0÷binding | 단계 손실 | 7% 판정 | raw 근거 |
|---|---:|---:|---:|---:|---:|---:|---|---|
| send-saturation 1K | 3 | 562,879.0 | 561,793.0 | 0.998 | 0.989 | −0.92% | 통과 | `raw/final/send-saturation-1K/run{1,2,3}/{zlink-java,zlink-framework-java}-send-saturation-1024/results.json` |
| send-saturation 4K | 3 | 207,805.6 | 204,971.8 | 0.986 | 0.979 | −0.75% | 통과 | `raw/final/send-saturation-4K/run{1,2,3}/{zlink-java,zlink-framework-java}-send-saturation-4096/results.json` |
| request-serial 1K | 3 | 14,578.4 | 14,878.8 | 1.021 | 0.998 | −2.27% | 통과 | `raw/final/request-serial-1K/run{1,2,3}/{zlink-java,zlink-framework-java}-request-serial-1024/results.json` |
| request-serial 4K | 5 | 13,785.6 | 13,808.2 | 1.002 | 0.971 | −3.16% | 통과 | `raw/final/request-serial-4K/run{1,2,3,4,5}/{zlink-java,zlink-framework-java}-request-serial-4096/results.json` |
| request-backpressure 1K | 3 | 141,669.2 | 142,676.2 | 1.007 | 1.007 | −0.01% | 통과 | `raw/final/request-backpressure-1K/run{1,2,3}/{zlink-java,zlink-framework-java}-request-backpressure-1024/results.json` |
| request-backpressure 4K | 3 | 112,333.4 | 114,198.6 | 1.017 | 1.042 | 2.44% | 통과 | `raw/final/request-backpressure-4K/run{1,2,3}/{zlink-java,zlink-framework-java}-request-backpressure-4096/results.json` |

L0 비율은 `/root/jobs/fx/cx/v21/s033/lad-java/report.md`의 #4350 표 값을 사용했다. L0 request-serial 4K의 0.971은 그 보고서의 3회 중앙값이다. 이번 L1와 binding request-serial 4K는 지정에 따라 각각 5회 측정했다.

## 복원한 코드와 reply parity

- `framework/bench/grpc/java/zlink-framework-server/src/main/java/systems/zlink/bench/withgrpc/frameworkserver/ZLinkFrameworkBenchServer.java:122-151`은 L1에서 Framework의 `ZLinkApplicationJobQueue`를 만들고 `acquireBlocking(REMOTE)`를 매 receive 전에 호출한다. 수신 뒤 `handlerStarted()`를 handler 호출 바로 앞에 두며, 반환 뒤 permit을 비워 중복 close를 하지 않는다. no-data·오류 경로에서는 예약 permit을 반환한다.
- 서버의 `ladderMode()` 분기 `ZLinkFrameworkBenchServer.java:63-65,186-188`은 L0/L1 모두 동일한 binding-shaped loop로 보내고, L1 환경변수에서 permit만 켠다.
- Framework bench client는 `FrameworkStack.java:56-67,125-130`과 `BenchClient.java:107-111,154-157`에서 L1도 기존 L0 `RawStack` operation을 유지한다. 요청에 permit을 더하지 않는다. ClientServer reply와 RouteMesh completion reply는 spec상 이 permit의 ordinary ingress가 아니다.
- binding reply는 `framework/bench/grpc/java/zlink-raw-server/src/main/java/systems/zlink/bench/withgrpc/rawserver/ZLinkRawBenchServer.java:115-127`이다. L0/L1 Framework bench reply `ZLinkFrameworkBenchServer.java:203-206`은 새 `RESPONSE_ENVELOPE`, 새 protobuf body encode, 두 파트 submit을 유지한다. request parts를 회신에 재사용하지 않는다.
- Framework queue의 acquire/permit 생성/동기 잠금은 `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/dispatch/ZLinkApplicationJobQueue.java:156-174,400-412`; 반환은 `:728-740`이다.
- Permit 판정 위치는 L0의 0곳에서 L1의 2곳으로 바뀌었다: receive 전 acquire 1곳, handler 직전 반환 1곳. L1 코드 검토에서 발견한 중복 `close()` 호출 1곳은 `15c8bab` 전에 제거했다.
- bench server/client 호출 경로 점검: 중복 반환 1건 수정, 그 밖의 성능·POSDDD·불필요한 기능 변경은 없음. 실행 gate 동기화는 추가하지 않았다.

## 빌드·측정 재현

- branch `exp/4350-java-ladder`; L1 커밋 `5e74ce818eeb09f731686ea2c5ac09e835af1e4a`, 중복 permit close 제거 커밋 `15c8bab1a664736766864416557f684edf13ac1c`. 모두 로컬 커밋이며 push/PR은 하지 않았다.
- `framework/bench/grpc/java/build.sh` 성공. 각 최종 측정 전 빌드, L1 client/server JAR이 관련 Java source보다 최신인지 확인한 뒤 `tools/run-group.sh` 사본으로 측정했다. 지정한 warmup 30초, active 5초, source CPU 0–7, target CPU 8–15, exclusive `/root/jobs/fx/cx/measure.lock`을 사용했다. 각 cell에서 binding과 L1을 같은 run-group session 안에서 측정했다. cross test는 하지 않았다.
- 측정 전 built commit, dirty flag, `origin/main` merge-base, worktree launcher/JAR SHA-256, binding/L1 reply 위치를 `progress.md`에 셀마다 한 줄 기록했다. 아티팩트와 SHA 상세는 그 파일에 있다.
- 최종 유효 raw는 `raw/final/` 아래 40개다. request-serial 4K는 각각 5회, 나머지는 각각 3회이며 모든 결과가 오류 0 및 카운터 일치다. `raw/l1/`의 앞선 두 셀 결과는 중복 permit close 수정 전 커밋 측정이어서 보존만 하고 표에서 제외했다. request-serial 4K의 첫 runner 호출도 실행기 경로 오류로 측정 전 중단되어 그 로그를 보존하고, 경로를 고친 5회 결과만 포함했다.
- 초과 손실 셀이 없어 추가 profile/lock 조사나 rebuild proposal은 필요하지 않았다.
