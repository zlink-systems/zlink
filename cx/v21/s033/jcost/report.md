Framework 원인 1, 대상 프로세스: Spring 핸들러 경로의 할당 가중치는 요청당 약 2,654바이트이며, 실행 표본은 요청당 2.06마이크로초입니다.
Binding 원인 1: Spring 핸들러 경로가 없어 해당 경로의 할당과 중앙 처리 장치 비용은 관측되지 않았습니다.
Framework 원인 2, 소스 프로세스: 공통 ForkJoinPool 제출 래퍼의 할당 가중치는 요청당 약 115바이트이며, 공통 풀 작업자 전체 시간은 요청당 19.1마이크로초입니다.
Binding 원인 2: 공통 ForkJoinPool 제출은 관측되지 않았으며, 대응 경로의 스케줄러 깨우기는 소스 입출력 작업자에서 요청당 0.175회입니다.
Framework 전체: 할당 표본 가중치 합계는 소스 요청당 약 44,281바이트, 대상 요청당 약 64,644바이트이며, 중앙 처리 장치 시간은 각각 74.1과 58.2마이크로초입니다.
Binding 전체: 할당 표본 가중치 합계는 소스 요청당 약 3,808바이트, 대상 요청당 약 11,258바이트이며, 중앙 처리 장치 시간은 각각 32.1과 30.8마이크로초입니다.

## 측정 범위와 해석

자료는 Framework와 Binding의 Java Flight Recorder 기록 8초, 스레드별 중앙 처리 장치 시간 및 스케줄러 추적 3초입니다. 요청 수 정규화에는 각각 Framework 소스 69,071건·대상 67,320건, Binding 소스 517,733건·대상 499,116건의 완료 수를 사용했습니다. Java Flight Recorder의 가중 할당 바이트는 표본 추정치입니다. 표본 사건 수는 실제 생성된 객체 수가 아니므로, 아래에 따로 쓴 핸들러 한 개 외의 정확한 객체 수는 이 자료만으로 복원할 수 없습니다. 8초 할당 추정치는 3초 동안의 요청 처리율이 기록 구간에도 유지됐다고 보고 환산한 값입니다.

Framework 캡처의 `sched.txt`는 비어 있고, `sched.data`는 perf에서 데이터 크기가 0이라고 보고해 해석할 수 없었습니다. 따라서 Framework 원인별 스케줄러 깨우기나 스레드 전달 횟수는 보고할 수 없습니다. Binding 스케줄러 추적은 읽혔지만, `sched_wakeup`은 깨어난 대상 스레드를 기록할 뿐 각 깨우기를 특정 요청 완료나 공통 풀 제출과 연결하지 않습니다. 이미 실행 중인 작업자에게 제출된 작업도 운영체제 깨우기로 나타나지 않을 수 있습니다. 그러므로 깨우기 수는 관측된 이름별 대상 스레드 깨우기의 비율이며, 제출·전달 횟수와 같다고 해석하지 않습니다.

## 원인별 근거와 비용 구성

원인 1은 Framework 대상의 요청 처리 중 Spring이 핸들러를 만들고 초기화한 뒤 폐기하는 경로입니다. 해당 호출은 [`ZLinkSpringHandlerFactory.constructHandler`](</root/worktree/zlink-ratio040/framework/languages/java/zlink-framework-spring-boot-starter/src/main/java/systems/zlink/framework/spring/ZLinkSpringHandlerFactory.java:488>)에서 생성자 호출, `autowireBean`, `initializeBean`을 수행하고, [`destroyOwned`](</root/worktree/zlink-ratio040/framework/languages/java/zlink-framework-spring-boot-starter/src/main/java/systems/zlink/framework/spring/ZLinkSpringHandlerFactory.java:244>)에서 Spring `destroyBean`과 필요한 `close`를 호출합니다. 측정 벤치마크의 형식은 `BenchEchoHandler`이며, 요청마다 핸들러 인스턴스 하나가 만들어집니다. Spring `RootBeanDefinition`, `BeanWrapperImpl`, `DisposableBeanAdapter`, `LinkedHashMap`, `ArrayList`, `CopyOnWriteArrayList` 반복자와 어노테이션 메타데이터 등의 표본이 생성·초기화 경로에 잡혔습니다.

이 경로에 겹치는 할당 표본은 대상 기록에서 386건, 가중치 합계 약 476,388,088바이트였습니다. 요청 처리율로 나누면 약 2,654바이트와 0.00215개 표본 사건/요청입니다. 표본 사건은 객체 개수가 아닙니다. 핸들러 한 개/요청은 호출 경로에서 확인되지만, 그 밖의 Spring 내부 객체 수는 추정할 수 없습니다. `ZLinkSpringHandlerFactory` 스택의 중앙 처리 장치 실행 표본은 37건으로, 10밀리초 표본 간격을 적용하면 요청당 약 2.06마이크로초입니다. 해당 표본은 대상 가상 스레드에 기록돼 이름 있는 운영체제 스레드별로는 나눌 수 없습니다. Binding에는 이 Spring 호출 경로가 없습니다.

원인 2는 Framework 소스의 서비스 완료 통지를 공통 ForkJoinPool에 전달하는 일입니다. [`ZLinkServiceOperationRegistry.complete`](</root/worktree/zlink-ratio040/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistry.java:404>)와 예외 완료 경로가 등록된 완료 항목을 [`ZLinkServiceCompletionDispatcher.post`](</root/worktree/zlink-ratio040/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceCompletionDispatcher.java:51>)에 넘기며, 이 함수는 `ForkJoinPool.commonPool().execute(item)`을 호출합니다. 요청이 성공 또는 예외로 완료될 때 완료 항목을 제출하는 경로는 각각 `completions.post(entry)`입니다. 각 제출에서 `ForkJoinPool.execute`가 `ForkJoinTask$RunnableExecuteAction` 래퍼를 만듭니다. 완료 항목 자체는 재사용되는 `WorkItem`입니다.

`ZLinkServiceCompletionDispatcher.post`를 포함하는 할당 표본은 소스 기록에서 11건, 가중치 합계 약 21,176,504바이트였고, 요청당 약 115바이트 및 0.000060개 표본 사건으로 환산됩니다. 이는 JFR 표본 가중치이지 객체별 실측 크기나 객체 개수는 아닙니다. 공통 풀 작업자 일곱 스레드의 중앙 처리 장치 시간 합계는 3초 구간에서 1.32초, 완료 요청당 약 19.1마이크로초였습니다. 이 합계에는 제출 이후의 응답 디코딩과 완료 연속 작업 실행도 들어가므로 제출 함수만의 비용으로 볼 수 없습니다. 제출 함수와 스택을 공유하는 실행 표본은 세 건으로, 10밀리초 간격 환산값은 요청당 약 0.16마이크로초이지만 표본 수가 작습니다. Binding에서는 이 디스패처와 공통 풀 제출 표본이 없으며, 소켓 입출력 작업자가 완료를 진행합니다.

## Binding에서 관측한 스케줄러 깨우기

Binding 소스의 이름 있는 입출력 작업자 `ZLINKbg/IO/0`과 `ZLINKbg/IO/2`는 각각 요청당 0.0807회와 0.0945회 깨워졌습니다. 합계는 요청당 0.1752회입니다. 대상의 `ZLINKbg/IO/0`과 `ZLINKbg/IO/1`은 각각 0.1230회와 0.0964회로 합계 0.2194회입니다. 대상 벤치마크 요청 스레드 `bench-raw-request`는 요청당 0.1263회 깨워졌습니다. 이는 3초 perf 스케줄러 자료에서 해당 이름을 가진 스레드를 대상으로 한 `sched_wakeup` 사건 수를 완료 요청 수로 나눈 값입니다. 다른 이름의 스레드와 벤치마크 구동 스레드 깨우기는 이 수치에 포함하지 않았습니다.

## 다른 주요 중앙 처리 장치 비용

아래 표는 perf 스레드 CPU 시간 차이를 같은 3초 구간의 완료 요청 수로 나눈 값입니다. 같은 이름으로 축약된 공통 풀 작업자들은 개별 스레드 행으로 표시했고, 원인 2의 합계는 일곱 작업자를 더한 값입니다. Framework 대상 Spring 비용은 위의 Java Flight Recorder 표본으로 별도 추정했으므로 이 표의 입출력 작업자 시간과 겹칠 수 있습니다.

| 순위 | 구현과 프로세스 | 이름 있는 스레드 | 요청당 중앙 처리 장치 시간 |
|---:|---|---|---:|
| 순위 | 구현과 프로세스 | 스레드 또는 스레드 합계 | 요청당 중앙 처리 장치 시간 |
|---:|---|---|---:|
| 1 | Framework 소스 | 공통 ForkJoinPool 작업자 7개 합계 | 19.11 마이크로초 |
| 2 | Framework 대상 | `ForkJoinPool-1` 작업자 3개 합계 | 17.38 마이크로초 |
| 3 | Framework 대상 | `ZLINKbg/IO/1` | 15.00 마이크로초 |
| 4 | Framework 소스 | `ZLINKbg/IO/2` | 14.04 마이크로초 |
| 5 | Framework 소스 | `bench-phase-active` | 13.46 마이크로초 |
| 6 | Framework 소스 | `ZLINKbg/IO/0` | 13.32 마이크로초 |
| 7 | Framework 소스 | `zlink-jvm-raw-mesh-bench` | 12.45 마이크로초 |
| 8 | Framework 대상 | `zlink-jvm-raw-mesh-bench` | 12.03 마이크로초 |
| 9 | Binding 소스 | `bench-phase-active` | 11.70 마이크로초 |
| 10 | Binding 소스 | `ZLINKbg/IO/0` | 10.84 마이크로초 |
| 11 | Binding 대상 | `ZLINKbg/IO/0` | 10.52 마이크로초 |
| 12 | Binding 대상 | `ZLINKbg/IO/1` | 10.16 마이크로초 |
| 13 | Binding 대상 | `bench-raw-request` | 9.22 마이크로초 |
| 14 | Binding 소스 | `ZLINKbg/IO/2` | 9.08 마이크로초 |

합산 항목은 개별 스레드 항목보다 여러 작업자의 시간을 더한 값입니다. Framework 대상 풀 합계는 9.51, 7.13, 0.74마이크로초인 세 작업자 시간의 합입니다. Framework 원시 메시 네트워크 처리, Framework 입출력과 벤치마크 구동, Binding 입출력 및 대상 벤치마크 요청 스레드의 시간이 두 원인 중 Spring 초기화 실행 표본 비용보다 큽니다. Framework 공통 풀의 19.11마이크로초는 두 번째 원인과 관련된 작업자 전체 비용이지만, 후속 연속 작업도 포함하므로 제출만의 비용이 아닙니다.

## 전체 할당 추정치와 점검

| 구현과 프로세스 | JFR 할당 표본 가중치 합계 | 요청당 가중 추정 바이트 | 표본 사건/요청 |
|---|---:|---:|---:|
| Framework 소스 | 8,156,071,192바이트 | 44,281바이트 | 0.01303 |
| Framework 대상 | 11,604,860,320바이트 | 64,644바이트 | 0.01334 |
| Binding 소스 | 5,257,327,240바이트 | 3,808바이트 | 0.00133 |
| Binding 대상 | 14,984,517,696바이트 | 11,258바이트 | 0.00115 |

전체 중앙 처리 장치 시간은 Framework 소스 요청당 74.1마이크로초, Framework 대상 58.2마이크로초, Binding 소스 32.1마이크로초, Binding 대상 30.8마이크로초입니다. 이 값은 운영체제가 기록한 스레드별 사용자 및 커널 중앙 처리 장치 시간 차이를 요청 수로 나눈 것이며, Java Flight Recorder 표본 추정치와는 측정 방법이 다릅니다.

코드 변경 없음. 리팩토링 점검은 적용하지 않았습니다.
