# #3871 Java request-serial 단계 계측

## 결과 (모든 계측값: 옛 조건(제출 thread 하나))

조건 B에서 1024 B request-serial을 같은 측정 세션으로 3회 실행했다. 각 칸은 `p50 / 평균` µs다. R4·R5·R6 원본은 [raw/corrected](./raw/corrected/)에 있다.

| 단계 | R4 | R5 | R6 |
|---|---:|---:|---:|
| **Framework source** caller submit → native send 반환 | 7.124 / 7.882 | 7.654 / 8.559 | 7.264 / 8.053 |
| reply 수신 → terminal claim | 0.090 / 0.113 | 0.090 / 0.107 | 0.110 / 0.124 |
| terminal claim → completion post | 0.040 / 0.043 | 0.040 / 0.046 | 0.040 / 0.044 |
| completion post → worker 시작 | 6.312 / 6.696 | 6.472 / 6.923 | 6.582 / 6.917 |
| worker 시작 → future 완료 호출 직전 | 1.964 / 2.420 | 1.933 / 2.402 | 2.034 / 2.450 |
| future 완료 호출 직전 → caller resume | 5.941 / 6.262 | 6.071 / 6.457 | 6.311 / 6.608 |
| **Framework source 전체 측정 구간** | 103.751 / 111.020 | 104.993 / 111.962 | 115.002 / 121.228 |
| **Framework target** handler 반환 → reply 제출 반환 | 5.339 / 6.268 | 5.260 / 6.506 | 6.401 / 7.157 |
| **Binding source** caller submit → native send 반환 | 3.266 / 3.859 | 3.176 / 3.733 | 3.156 / 3.747 |
| native send 반환 → caller resume (Binding 내부는 한 구간) | 61.143 / 65.456 | 61.653 / 64.794 | 63.808 / 65.393 |
| **Binding source 전체 측정 구간** | 64.619 / 69.316 | 64.889 / 68.527 | 67.264 / 69.140 |

Framework source 단계 평균의 합은 약 **24.0 µs**다. 그중 caller submit→send 반환이 8.05 µs, claim/post/worker 구간이 약 9.49 µs, future 완료→caller 재개가 6.46 µs다. 따라서 이전에 남았던 약 24 µs와 비슷한 크기의 source 경로 비용이 관찰되지만, 이전 보고의 residual과 이번 합계는 경계가 같지 않아 전부 동일한 비용이라고 단정하지 않는다. Target 진입 전 wire·admission 구간은 이번 스탬프 범위 밖이다. future 완료 경계는 `Operation.receiveValue`의 `complete(decoded)` 호출 직전 스탬프다([ZLinkServiceOperationRegistry.java:859](/root/worktree/zlink-v21-s033/i3871b/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistry.java:859)). 단계 경계는 [stamps.patch](./stamps.patch)와 해당 호출 위치([BenchDrivers.java:480](/root/worktree/zlink-v21-s033/i3871b/framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/BenchDrivers.java:480), [ZLinkServiceOperationRegistry.java:407](/root/worktree/zlink-v21-s033/i3871b/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistry.java:407), [ZLinkServiceCompletionDispatcher.java:51](/root/worktree/zlink-v21-s033/i3871b/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceCompletionDispatcher.java:51), [ZLinkMeshApplicationDispatcher.java:678](/root/worktree/zlink-v21-s033/i3871b/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkMeshApplicationDispatcher.java:678))로 확인할 수 있다.

무계측 Framework 셀은 p50 110 µs, 평균 110.419 µs였다([baseline 결과](./raw/baseline/round0/zlink-framework-java-request-serial-1024/results.json)). 계측 셋의 평균 RTT 중앙값은 112.131 µs로 관측 차이는 **+1.712 µs**다. 무계측 셀은 1회뿐이므로 이 차이를 스탬프의 인과 비용으로 분리하지는 못한다.

Binding에는 Framework의 terminal claim과 dispatcher 경계가 없다. 그래서 같은 caller submit/send 경계와 native send 반환부터 caller resume까지를 비교했다. raw reply의 수신·검증·완료는 두 번째 구간에 함께 들어간다([RawStack.java:76](/root/worktree/zlink-v21-s033/i3871b/framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/RawStack.java:76)).

## 결정 소유 표 (요청 completion)

공통 계약은 잠금 밖에서 process-shared completion dispatcher에 게시하고 새 execution turn에서 실행하도록 정한다([S01 §11:492](/root/worktree/zlink-v21-s033/i3871b/framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:492), [S01 §11:501](/root/worktree/zlink-v21-s033/i3871b/framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:501)). 실행 자원 배치는 수신 역할 분리와 언어별 자원 표가 정한다([S08 §4.4:302](/root/worktree/zlink-v21-s033/i3871b/framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md:302), [S08 자원 표:374](/root/worktree/zlink-v21-s033/i3871b/framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md:374)).

| 언어 | terminal 이후 결정 위치와 게시 경로 | 실행 자원 및 언어별 계약 근거 |
|---|---|---|
| C++ | `operation_registry.cpp:269`가 completion을 `dispatch_completion(false, owner, dispatcher)`로 보낸다. [구현](/root/worktree/zlink-v21-s033/i3871b/framework/languages/cpp/framework/src/runtime/foundation/operation_registry.cpp:269). `provider_task_context.cpp:174`이 허용된 현재 turn 재사용 여부를 판정하고, 아니면 dispatcher에 게시한다. [구현](/root/worktree/zlink-v21-s033/i3871b/framework/languages/cpp/framework/src/runtime/dispatch/provider_task_context.cpp:174). | 전용 process-shared `completion_dispatcher_t`; worker 수는 최소 2, 상한은 감지된 processor 수다([provider_task_context.cpp:56](/root/worktree/zlink-v21-s033/i3871b/framework/languages/cpp/framework/src/runtime/dispatch/provider_task_context.cpp:56)). 언어 계약의 completion transition 계측은 [C++ README:39](/root/worktree/zlink-v21-s033/i3871b/framework/doc/framework/common/spec/server/languages/cpp/README.ko.md:39)에 있다. |
| .NET | reply terminal은 `ZLinkMeshCompletionTable.TryComplete`에서 pending을 꺼낸 뒤 `Post`한다([ZLinkMeshCompletionTable.cs:190](/root/worktree/zlink-v21-s033/i3871b/framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkMeshCompletionTable.cs:190)); fail/cancel도 같은 post를 사용한다([:207](/root/worktree/zlink-v21-s033/i3871b/framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkMeshCompletionTable.cs:207), [deadline post:311](/root/worktree/zlink-v21-s033/i3871b/framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkRequestDeadlineIndex.cs:311)). | `ZLinkCompletionDispatcher.Post`가 `ThreadPool.UnsafeQueueUserWorkItem(..., preferLocal: false)`에 게시한다([ZLinkCompletionDispatcher.cs:13](/root/worktree/zlink-v21-s033/i3871b/framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkCompletionDispatcher.cs:13)). 전용 worker가 아닌 process ThreadPool이다. [언어 계약 계측](/root/worktree/zlink-v21-s033/i3871b/framework/doc/framework/common/spec/server/languages/dotnet/README.ko.md:52). |
| Java | request terminal claim 후 `completions.post(entry)`를 실행한다([ZLinkServiceOperationRegistry.java:407](/root/worktree/zlink-v21-s033/i3871b/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistry.java:407)); failure와 deadline 경로도 동일 dispatcher에 post한다([:419](/root/worktree/zlink-v21-s033/i3871b/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistry.java:419), [expiry:635](/root/worktree/zlink-v21-s033/i3871b/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistry.java:635)). | `ZLinkServiceCompletionDispatcher.post`는 매 항목을 `ForkJoinPool.commonPool().execute(item)`에 넣는다([ZLinkServiceCompletionDispatcher.java:51](/root/worktree/zlink-v21-s033/i3871b/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceCompletionDispatcher.java:51)). 즉 Java만은 아니다. .NET도 매 terminal을 process 공유 ThreadPool에 보낸다. [언어 계약 계측](/root/worktree/zlink-v21-s033/i3871b/framework/doc/framework/common/spec/server/languages/java/README.ko.md:39). |
| Node | `OperationRegistry.complete`가 terminal을 꺼내고 observer 또는 caller Promise를 완료한다([operation-registry.ts:332](/root/worktree/zlink-v21-s033/i3871b/framework/languages/node/packages/framework/src/runtime/foundation/operation-registry.ts:332)). raw service의 observer·미생성 Promise 경로는 `queueMicrotask`를 명시하고, 일반 Promise 경로는 resolve한다([raw-service-mesh-runtime.ts:783](/root/worktree/zlink-v21-s033/i3871b/framework/languages/node/packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts:783)). | 전용 worker/pool 없이 event loop에서 Promise continuation microtask로 재개된다. Node 언어 계약은 완료 continuation이 같은 event loop에서 실행됨을 명시한다([README:29](/root/worktree/zlink-v21-s033/i3871b/framework/doc/framework/common/spec/server/languages/node/README.ko.md:29)). |

정리하면 Java만 shared pool을 쓰는 것은 아니다. .NET은 process ThreadPool, C++은 자체 공유 dispatcher worker, Java는 `ForkJoinPool.commonPool`, Node는 event-loop microtask를 사용한다. C++만 자체 dispatcher pool이며 현재 구현은 worker 최소 수를 2로 둔다. PM 설계 판단은 위 소유 경계에 맡긴다.

## 검증

- 빌드: `ZLINK_CORE_SOURCE=release ZLINK_CORE_PACKAGE_PREFIX=/root/.cache/zlink/core-release/1.24.0/linux-x64 bash framework/bench/grpc/java/build.sh`; 로그 [build.log](./raw/build.log), 보정 빌드 [build-corrected.log](./raw/build-corrected.log).
- 측정: 1024 B, warmup 30초·active 5초, source CPU 0–7 / target CPU 8–15. `flock -o /root/jobs/fx/cx/measure.lock` 아래 순차 실행했다. 로그 [measurement-corrected.log](./raw/measurement-corrected.log); 셀별 `results.json`, before/after snapshot 및 source/target stamp CSV는 [raw/corrected](./raw/corrected/)에 있다. 무계측 셀은 [raw/baseline](./raw/baseline/)에 있다.
- 계측은 `System.nanoTime()`과 JVM별 사전 할당 배열을 사용했고 hot path에서 파일/문자열 로깅을 하지 않았다. 임시 diff는 [stamps.patch](./stamps.patch)에 보존하고 코드 변경을 역적용했다. build 및 `.gradle` 출력 19개 디렉터리를 제거했고 `git status --short`는 비어 있다. 테스트는 실행하지 않았다.
- 첫 시도는 `snapshot.py`가 없어 active 측정 전에 실패했다. 이후 retry 결과로 첫 시도의 partial warmup 파일이 덮어써졌음을 [first-attempt-failure.log](./raw/first-attempt-failure.log)에 기록했다. 최종 corrected raw에는 이 문제가 없다.

리팩토링 점검: 성능 0건, POSDDD 0건, 불필요 코드 0건 발견. 고친 항목 0, 넘긴 항목 없음.

## 남은 문제

- 무계측 baseline은 1회라서 +1.712 µs 차이는 관측 비교값이며 stamp 비용의 인과 추정은 아니다.
- 이번 stamp 경계는 target wire/admission 구간을 분리하지 않는다. 이전 residual +24 µs와 이번 source span 합계의 일치는 크기 비교이며 완전한 원인 분해는 아니다.
