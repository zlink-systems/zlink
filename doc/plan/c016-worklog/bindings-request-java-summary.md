# Java REQUEST 계약 통일 요약

## 결과

- 비동기 REQUEST가 Core의 단일 DONTWAIT admission 결과를 처리한다.
- 즉시 admission 성공 시 기존 REQUEST completion ID로 reply/timeout을 기다린다.
- `BACKPRESSURED/EAGAIN`이면 그 시점에만 Java `Message` shared snapshot을 만들고, 해당 token/context/RID의 `WRITABLE`에서만 같은 REQUEST를 재제출한다.
- 재제출이 다시 거절되면 새 token을 arm하며, admission 뒤 snapshot을 즉시 해제하고 REQUEST completion으로 전환한다.
- TERMINAL WRITABLE의 `ENOENT`는 `ZlinkRequestException(NOT_FOUND)`, `ECANCELED/ESHUTDOWN/ETERM`은 `ZlinkRequestException(TERMINATED)`로 전달한다.
- blocking REQUEST(NONE)는 기존 동작을 유지한다. spin/sleep/timer를 추가하지 않았고 기존 POLLCOMPLETION runtime/public owner를 wake source로 사용한다.
- `PENDING_MAX_MSGS/BYTES`는 enum과 option mapping을 ABI 호환용으로 유지하되 무시되는 옵션으로 문서화했다.
- multi REQREP perf runner는 폐기된 “동기 admission 거절까지 계속 제출” 모델을 제거하고 클라이언트당 REQUEST completion 1개만 진행하도록 수정했다.

## API 전/후

| 구분 | 변경 전 | 변경 후 |
|---|---|---|
| `RequestSubmitOperation.submit()` | admission 전에 pending REQUEST를 등록하고 BACKPRESSURED를 동기 `ZlinkSubmitException`으로 노출 | BACKPRESSURED token을 binding이 소유해 WRITABLE에서 재제출하고 최종 reply/timeout/terminal stage로 완료 |
| payload ownership | Core pending-admission 보관을 전제 | 최초 거절 시에만 binding shared snapshot 생성, admission 직후 해제 |
| timeout | pre-admission pending 상태 전제 | Core admission 성공 시점부터 기존 REQUEST timeout 시작 |
| `submit_sync()` | Core NONE blocking REQUEST | 변경 없음 |
| `PENDING_MAX_*` | REQUEST pending-admission 제한으로 설명 | ABI 값만 유지, SEND/REQUEST 모두 무시 |

공개 Java 메서드 시그니처 변경은 없다.

## 변경 파일

- `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java`
- `bindings/java/src/main/java/systems/zlink/contracts/errors/Errors/ZlinkException.java`
- `bindings/java/src/main/java/systems/zlink/contracts/messaging/RequestOperation.java`
- `bindings/java/src/main/java/systems/zlink/contracts/messaging/RequestSubmitOperation.java`
- `bindings/java/src/main/java/systems/zlink/internal/sockets/SocketOption.java`
- `bindings/java/src/main/java/systems/zlink/internal/sockets/SocketOptions.java`
- `bindings/java/src/test/java/systems/zlink/integration/contract/RequestWaitTokenContractTest.java` (신규)
- `bindings/java/src/test/java/systems/zlink/contract/RoutedMultipartAdmissionContractTest.java`
- `bindings/java/perf/multi/Zlink.BindingBench.Multi/src/main/java/systems/zlink/perf/multi/PerfMultiSocketReqRep.java`
- `bindings/java/README.javadoc.md`

## 테스트

- REQUEST 공개 API 회귀: PASS. HWM fill → token wait → drain/reply → WRITABLE retry → reply를 sleep 없이 5회 반복했다.
- connect-before-bind REQUEST: PASS.
- socket close의 REQUEST waiter typed termination/정리: PASS.
- SEND token과 REQUEST token 혼재: PASS.
- `RequestWaitTokenContractTest`: 최종 8 tests PASS(이 중 HWM 케이스 5 repetitions).
- `:test`: 최종 90 tests PASS.
- `:integrationTest`: PASS.
- `:zlink-ext-netty:test`: PASS.
- `:kotlin-contract-test:test`: PASS.
- `:perf-multi:test`: PASS.
- samples: JDK 22를 명시한 단일 Gradle invocation에서 7/7 PASS.
- `git diff --check`: PASS.

`bash bindings/java/tests/run_tests.sh` 최초 실행에서는 변경 전 가정을 가진 `RoutedMultipartAdmissionContractTest` 1건이 실패해 새 계약으로 수정했다. 같은 실행의 integration/netty/kotlin은 통과했다. 샘플 wrapper는 class 66 산출물을 시스템 JDK 21(class 65)로 실행해 실패했으며, JDK 22 launcher를 명시한 동일 7개 sample task는 모두 통과했다.

## 스모크 수치

### single

조건: tcp, 1024B, duration 2, runs 1. 결과 파일: `bindings/java/perf/results/single/report/perf_java_single_linux_20260905_003849_request-contract.txt`

| 패턴 | throughput | status |
|---|---:|---|
| DEALER_ROUTER_REQREP | 220.5 ops/s | complete, nonzero |
| ROUTER_ROUTER_REQREP | 230.0 ops/s | complete, nonzero |
| DEALER_ROUTER | 167,433.5 msg/s | complete, nonzero |

### multi

조건: clients 8, duration 2, sizes 1024/65536, transports tcp/tls/ws/wss, runs 1. 최종 수정 runner 결과 파일: `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260905_005007_request-contract-final.txt`

- 24/24 success, unsupported 0, fail 0, status complete, 모든 throughput nonzero.
- DEALER_ROUTER_REQREP: 1,282.0~2,773.5 ops/s(transport/size 전체).
- ROUTER_ROUTER_REQREP: 891.0~3,840.0 ops/s.
- DEALER_DEALER: 4,886.0~236,711.0 msg/s.

## BLOCKERS

- 코드/계약 blocker 없음.
- 환경 주의: sample wrapper가 시스템 JDK 21을 고르면 class-version 오류가 발생한다. JDK 22 launcher 지정 시 7/7 통과한다.
- perf wrapper의 source timestamp gate는 symlink된 승인 Core runtime보다 48초 늦은 local header mtime 때문에 실행을 거부했다. Core는 금지사항에 따라 재빌드/clean/touch하지 않았고, 동일 wrapper에 일시적 gate 우회를 적용해 측정한 뒤 해당 변경을 되돌렸다. 최종 runner/report는 지정된 `core/build/lib/libzlink.so.0.17.0`을 사용했다.
