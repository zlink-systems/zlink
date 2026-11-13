# Java binding submit 비용 측정 (#4060)

## 설정과 로드된 binding

main은 `origin/main`의 지정 커밋 `6b24e01426c998b0b270d49c8a72a2e48f605317`에서 만든 detach scratch worktree로 측정했다. PR은 `/root/worktree/zlink-4060-binding-close`의 `b42f97845111c37d74d7b5315440611b16516d21`에서 측정했다. 양쪽 Java bench와 binding은 각각의 worktree 소스를 사용해 `/root/jobs/fx/cx/measure-shared.sh bash ./build.sh`로 빌드했다. Core 1.23.0 설치 prefix와 native library 경로는 두 쪽에서 동일했다.

bench의 `settings.gradle.kts`는 Java Framework를 composite build로 포함한다. Framework Java의 `gradle/zlink-local-packages.settings.gradle.kts`는 `ZLINK_JAVA_BINDINGS_SOURCE`를 composite build에 포함하고 `systems.zlink:zlink` 의존성을 그 빌드의 `:` 프로젝트로 치환한다. 각 측정은 해당 side의 binding source 경로를 환경 변수로 지정했다. 실행된 raw server의 installDist classpath는 `lib/*`를 사용하며, 실제 로드 경로와 빌드 산출물 hash가 일치했다.

main의 로드 jar는 `/root/jobs/fx/cx/v21/s033/jbind/main/framework/bench/grpc/java/zlink-raw-server/build/install/bench-zlink-raw-server/lib/zlink-java-1.23.0.jar`이고 SHA-256은 `30c6dd5b726c68d7729dac4034c31b4afa9a8aeeff13642f36a9af4045648f1f`다. 같은 hash의 binding build jar는 `/root/worktree/zlink-jbind-main/bindings/java/build/libs/zlink-java-1.23.0.jar`다.

PR의 로드 jar는 `/root/jobs/fx/cx/v21/s033/jbind/pr/framework/bench/grpc/java/zlink-raw-server/build/install/bench-zlink-raw-server/lib/zlink-java-1.23.0.jar`이고 SHA-256은 `df0ac6f55a455d684fefbc5f2b512bfd7f86cafd0cb7951fcb9cd3147bb97150`다. 같은 hash의 binding build jar는 `/root/worktree/zlink-4060-binding-close/bindings/java/build/libs/zlink-java-1.23.0.jar`다. main과 PR jar hash가 달라 각 측정이 다른 binding build를 사용했음을 확인했다.

원본 runner에는 CPU affinity 설정이 없어 jbind 안의 scratch bench 복사본에서 `taskset`으로 source JVM을 CPU 0–7, target JVM을 CPU 8–15에 고정했다. JVM main class를 찾아 `taskset -pc`로 확인했으며 10개 유효 실행의 모든 셀에서 source 0–7, target 8–15가 기록됐다. 빌드에 필요한 `proto/bench.proto`도 scratch 복사본에 포함했다. tracked 파일은 수정하지 않았다. 처음 두 affinity 확인 시도는 wrapper PID만 확인해 표본에서 제외했고, 실제 JVM PID를 확인하는 scratch 기록을 추가한 뒤 아래 5회 표본을 다시 수집했다.

유효 실행 순서는 main 1회, PR 1회, PR 1회, main 1회, main 1회, PR 1회, PR 1회, main 1회, main 1회, PR 1회다. 각 호출은 `flock -o /root/jobs/fx/cx/measure.lock` 안에서 실행했고 lock은 측정 실행 동안만 보유했다. 셀은 `zlink-java`, payload 1024 B, warmup 2초, active 5초, request window 100, send concurrency 8이었다. 측정 중 관찰한 MemAvailable 최저값은 20.7 GiB였다. 각 결과에서 active duration은 5초이고 client 및 server 오류는 0이었다.

## 결과

처리량은 ops/s다. 표본은 각 side에서 시간 순서로 적었다. 처리량 환산 시간은 `1,000,000 / median ops/s`로 계산했으며, 시간 차이는 PR에서 main을 뺀 값이다. 이는 throughput의 역수 환산값이다. run-to-run spread 밖인지는 두 side의 5개 표본 최솟값–최댓값 구간이 서로 겹치지 않는지로 판단했다.

| 셀 | main 표본 1–5 (ops/s) | main 중앙값 | PR 표본 1–5 (ops/s) | PR 중앙값 | 처리량 차이 (PR 대비 main) | 환산 시간 차이 (PR−main) | spread 밖 |
|---|---:|---:|---:|---:|---:|---:|---|
| request-serial | 13,623; 14,394; 9,557; 8,740; 9,748 | 9,748 | 13,931; 13,641; 9,244; 9,289; 9,741 | 9,741 | -0.07% | +0.072 µs/request | 아니오, 구간 겹침 |
| request-backpressure | 127,748; 127,139; 89,058; 90,756; 90,118 | 90,756 | 123,850; 120,884; 85,350; 90,697; 92,157 | 92,157 | +1.54% | -0.167 µs/request | 아니오, 구간 겹침 |
| send-saturation | 300,578; 303,868; 210,857; 218,838; 229,157 | 229,157 | 299,842; 301,390; 219,926; 240,471; 240,983 | 240,983 | +5.16% | -0.214 µs/send | 아니오, 구간 겹침 |

세 셀 모두 main과 PR의 5회 처리량 범위가 겹쳐 run-to-run spread 밖의 차이는 관측되지 않았다. 판정은 request-serial 변화 측정 불가, request-backpressure 변화 측정 불가, send-saturation 변화 측정 불가다. 관측된 median 차이만으로 PR의 submit 비용 증가를 확인할 수 없었다.

벤치는 publish pattern을 제공하지 않는다. 허용 패턴은 request-serial, request-backpressure, send-saturation이며, publish 비용의 대리 셀로 PM이 동의한 send-saturation을 포함했다.

벤치에는 목표 rate를 설정하는 pacing 입력이 없다. request window와 send concurrency는 고정값이고 driver는 포화 방식으로 진행하므로, common paced rate에서 source와 target의 user/kernel CPU per request를 측정하지 않았다.

측정 결과와 affinity 로그는 `/root/jobs/fx/cx/v21/s033/jbind/results/` 및 `/root/jobs/fx/cx/v21/s033/jbind/log-jbind-*.txt`에 보존했다. Scratch main worktree는 측정 종료 후 제거했다.
