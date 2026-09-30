# #1184 B8 Java send timeout 설정 경로

## 원인과 수정

기준 구현에는 Java 공개 builder의 채널별 send timeout 설정이 없었습니다. `origin/main`의 `ZLinkJavaChannelBackendAdapter.java:24,40`은 DEALER·PUB 생성 때 `DEFAULT_SEND_TIMEOUT`을 직접 적용했고, `ZLinkChannelSocketRegistry.java:317,371-373`도 준비 대기 상한에 같은 상수를 별도로 사용했습니다. 이에 따라 사용자 설정을 SNDTIMEO와 준비 대기 양쪽에 전달할 경로가 없었습니다. 수정 전 공개 builder·adapter 호출 재현은 `job-logs/pre-fix-repro-2.log`에 있으며, 새 setter와 timeout 인자형 factory가 없다는 컴파일 오류 9건으로 실패했습니다.

`ZLinkClientServerChannelClientBuilder`와 `FanoutChannelBuilder`에 `setSendTimeout(Duration)`을 추가했습니다. 값은 `ChannelRegistration` 한 곳에서 1초 기본값을 적용하고, millisecond 올림 및 `1..Integer.MAX_VALUE` 검증을 거칩니다. 등록값은 수동·자동·process-local ClientServer DEALER와 classic fanout PUB의 socket 생성에 전달하며, ClientServer 준비 대기는 같은 등록값을 사용합니다. 등록이 없는 기존 제출 경로의 1초 대기 간격도 유지했습니다. SNDTIMEO는 socket 생성 때 한 번 설정하고, 호출별 socket option 변경은 추가하지 않았습니다.

중복된 millisecond 검증·정규화는 기존 `ZLinkChannelAdmissionTimeout`으로 모았습니다. Java topology reference의 한국어·영어 문서에 두 builder 설정과 허용 범위를 추가했습니다. Server options guide는 common guide에서 생성된 파일이므로 수정하지 않았습니다.

## 변경 위치

| 변경 | 위치 |
|---|---|
| 공개 설정 API와 builder 위임 | `ZLinkClientServerChannelClientBuilder.java:11`, `FanoutChannelBuilder.java:27`, `ChannelBuilders.java:67,253` |
| 기본값·검증·올림 정규화 | `ChannelRegistration.java:38,179,183-184`, `ZLinkChannelAdmissionTimeout.java:9,13-38` |
| SNDTIMEO 적용과 socket 생성 경로 | `ZLinkJavaChannelBackendAdapter.java:28-32,50-54`, `ZLinkChannelRuntimeConfigurator.java:60,96`, `ZLinkChannelRuntime.java:824,851,857-859`, `ZLinkClientServerLocationRuntime.java:491` |
| 준비 대기 상한과 기존 미등록 경로 기본값 | `ZLinkChannelSocketRegistry.java:293-296,323,374` |
| 검증 테스트 | `ZLinkChannelAdmissionTimeoutTest.java:36-109`, `ZLinkChannelRuntimeTest.java:91-107,2220-2285`, `ZLinkJavaChannelSendTimeoutTest.java:27-41` |
| Java 설정 문서 | `framework/doc/framework/java/reference/02-topology-discovery.ko.md:131-147,164-193`, `framework/doc/framework/java/reference/02-topology-discovery.en.md:137-153,173-203` |

## 재현과 검증

| 단계 | 로그 | 결과 |
|---|---|---|
| 수정 전 공개 API 재현 | `job-logs/pre-fix-repro-2.log` | `setSendTimeout`과 timeout 인자형 socket factory가 없어 컴파일 실패(9 errors) |
| 설정·registry·native socket 관련 테스트 | `job-logs/focused-after-review.log` | `ZLinkChannelAdmissionTimeoutTest`, `ZLinkChannelRuntimeTest`, `ZLinkJavaChannelSendTimeoutTest` 통과 |
| Java framework 전체 unit·contract | `job-logs/java-full-final.log` | `test contractTest` 성공, 63 actionable tasks |
| format check | `job-logs/format-check-final.log` | `scripts/format/format.sh java --check`, `FORMAT_EXIT=0` |
| local package·Node workspace 준비 | `job-logs/node-httpclient-pack-3.log`, `job-logs/node-workspace-install-3.log`, `job-logs/node-workspace-build-3.log` | Java Maven 1.12.0 local package와 Node workspace 준비 성공 |

전체 테스트 명령은 WSL worktree 사본에서 다음과 같이 실행했습니다.

```sh
JAVA_HOME=/usr/lib/jvm/jdk-25.0.4.1+1 \
ZLINK_LOCAL_PACKAGE_ROOT=/mnt/d/t/jobs/fx/b8-3/packages \
./gradlew --no-daemon --max-workers=2 test contractTest
```

최종 전체 검증과 관련 테스트에서 실패는 없었습니다. 따라서 `origin/main` 대조 실행은 필요하지 않았습니다.

## 감독자 리뷰 기준 일곱 가지

| 기준 | 판정 |
|---|---|
| 스펙 gap | 없음. 값·socket option·준비 대기·NotFound/DeadlineExceeded 규칙은 `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:223-240,545`와 `framework/doc/framework/common/spec/server/00-foundation/04-interaction-model.ko.md:189-220`에 근거합니다. |
| 불필요한 규칙 | 새 runtime flag·timer·retry·queue·lock은 없습니다. 설정값 저장용 `ChannelRegistration.sendTimeout` 1개를 추가하고, 중복된 mesh timeout 검증과 spot publisher millisecond 정규화를 공통 helper로 옮겼습니다. |
| 제어 분산 | 같은 timeout 선택을 하던 adapter 기본값과 registry 기본값을 등록 단계의 정규화값 하나로 모았습니다. socket adapter와 readiness registry는 해당 값을 적용·소비합니다. |
| 동기화 | 새 동기화나 대기는 없습니다. 기존 state lane 제출과 lane 밖 readiness 대기를 유지하며, 등록은 제출 수락 전에 확정됩니다. |
| hot path | native SNDTIMEO 설정은 socket 생성 때 한 번 수행합니다. 메시지별 옵션 변경, 추가 native 호출, lock, 큐 순회는 없습니다. |
| 리팩토링 잔여 | 검토한 Java 설정·socket factory·readiness 호출 경로에서 추가 POSDDD·성능·불필요 코드 수정 사항은 찾지 못했습니다. |
| 계층 소유 | Framework는 설정값 전달과 ClientServer 준비 대기를 담당합니다. Core의 요청 수락·연결 admission 동작을 복제하거나 재판정하지 않았습니다. |

판정 위치 수는 수정 전 2곳(adapter SNDTIMEO 기본값, registry readiness 기본값)에서 수정 후 1곳(`ChannelRegistration`의 유효값 정규화)으로 줄었습니다. 등록이 없는 기존 내부 제출 경로는 공통 1초 상수로 이전 대기 간격을 보존합니다.

## 소유 계층·교차 언어·분류

- **소유 계층:** Java Framework registration이 설정값과 기본값을 소유하고, binding adapter가 socket 생성 때 SNDTIMEO를 적용하며, channel registry가 같은 값으로 ClientServer 준비를 기다립니다. Core는 요청 수락 이후 timeout을 소유합니다.
- **스펙 조항:** `01-submit-and-completion.ko.md:223-240`의 send timeout 값과 socket 설정 규칙, `:545`의 request timeout 시작점, `04-interaction-model.ko.md:189-220`의 admission 결과 규칙을 따릅니다.
- **교차 언어 대조:** .NET `ZLinkSocketConfig.NormalizeSendTimeout`·`ZLinkBackendSocketOptionsMapper`는 유효 범위와 socket option 전달을 담당하고, `ZLinkClientServerClientRuntime.WaitForReadyAsync`는 send timeout으로 준비를 제한합니다. Node의 `sendTimeoutMs` 검증·socket 적용 경로도 확인했습니다. Java는 같은 유효 범위와 기본값을 사용하고, 등록 단계의 단일 `Duration`을 socket 생성과 readiness bound에 함께 전달합니다.
- **변경 분류:** A — 정해진 공통 계약에 Java 설정 경로를 맞춘 계약 적응입니다. 스펙 변경이나 동작 gap 판정은 없습니다.
