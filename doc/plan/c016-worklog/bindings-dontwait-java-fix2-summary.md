# bindings-dontwait-java-fix2 요약

## 결과

- REQUEST 경로를 HEAD 동작에 맞게 복원했다.
- 수정 파일: `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java`
- `submitRequest`와 `submitRequestWithFlags`에는 `drainLock`을 다시 넣지 않았다. `nativeCallGate`도 동시 획득 가능한 read lock을 유지한다.
- Contract B의 DONTWAIT SEND 단일 시도, wait token, WRITABLE completion, 동일 payload 재전송 경로는 변경하지 않았다.

## 원인과 수정

- 포팅 과정에서 REQUEST pending 등록 직후 실행하던 runtime completion owner 준비가 multipart native 제출 뒤로 이동했다. 이 순서 변화가 HEAD의 pre-admission 실행/경합 창을 없애 concurrent 256-part REQUEST들이 Core owner-thread rejection을 노출하지 않는 회귀를 만들었다.
- `registerRequest()`가 pending 등록 직후 `startRuntimeOwner()`를 호출하도록 복원하고, 성공 뒤의 늦은 owner 시작 호출을 제거했다.
- send용 `submitFailure()`는 token 없는 `BACKPRESSURED/EAGAIN`을 Contract B invariant 위반으로 `INTERNAL_ERROR` 처리한다. REQUEST에는 wait-token SEND 계약이 적용되지 않으므로 `requireRequestSuccess()`를 분리해 Core의 `SubmitResult`와 errno를 그대로 `ZlinkSubmitException`으로 보존했다.
- `ZLINK_ROUTED_PART_DEBUG=1` 진단에서 `prepare_send_step busy ... same_thread=0`가 관찰되어 Native ABI/rc 전달이 아니라 Java REQUEST orchestration 회귀임을 확인했다. 임시 소스 로그는 남기지 않았다.

## 검증

- `RoutedMultipartAdmissionContractTest.concurrentMultipartRequestsExposeCoreRejectionAndBindingStagingRetainsOwnership` + `DontWaitBackpressureContractTest` + `CompletionKindContractTest`: 한 실행 묶음으로 5회 연속 통과.
- 로컬 Core 전체 `:test`: `tests=90 skipped=0 failures=0 errors=0`.
- `git diff --check -- bindings/java`: 통과.
- 메모리 제한 `ulimit -v 16777216`과 지정된 local Core include/lib를 모든 gate에 적용했고 Gradle worker 수를 늘리지 않았다.

## 작업트리 상태

- 작업 중 외부 감독 프로세스가 `main`을 `7927c582c2` (`bindings/java: adopt the 0.17.0 DONTWAIT wait-token / WRITABLE completion contract`)로 전진시켰고, 위 수정도 해당 커밋에 포함됐다. 이 작업에서는 commit/push/reset/checkout/stash를 실행하지 않았다.

## BLOCKER

- samples는 재실행하지 않았다. 시스템 `java`는 OpenJDK 21.0.12이고 `bindings/java/build.gradle` toolchain은 Java 22여서, 보고된 `UnsupportedClassVersionError`는 환경 JDK 불일치로 판단한다. 금지된 build/sample 설정은 수정하지 않았다.
