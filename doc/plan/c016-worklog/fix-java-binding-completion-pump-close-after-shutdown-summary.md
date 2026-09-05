# Java binding — shutdown 이후 completion pump 해제 수정 결과

2026-09-05. `context.shutdown(); context.close();`의 completion control send 실패를
Java binding에서 수정했다. 새 binding class로 공개 API 재현이 통과했고, 지정된 Gradle
`test`도 성공했다. `main`의 변경은 commit하지 않았다.

## 원인과 수정

수정 전 `bindings/java/src/main/java/systems/zlink/runtime/core/NativeContext.java:280`은
native shutdown 전에 `completionDispatcher.closeNativeWait()`를 호출했다.
`bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionPump.java:222,327`은
pump close 때 control socket으로 send했다. 호출자가 이미 `shutdown()`을 실행했으면
이 send가 `SubmitResult.TERMINATED`, errno `156384765`로 실패하여 join과 자원 해제를
완료하지 못했다. 기존 증거는 `/tmp/java-bingo-fix-evidence/bingo-diagnosis-roles/session-b.log:499–504`와
같은 디렉터리의 `ContextShutdownRepro.java`다.

- [NativeContext.java:280](../../../bindings/java/src/main/java/systems/zlink/runtime/core/NativeContext.java#L280):
  Context가 native shutdown을 수행한 뒤 pump를 join·해제하고 native term을 실행한다.
- [CompletionPump.java:215](../../../bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionPump.java#L215):
  close의 control send를 제거했다. shutdown이 기존 native wait를 종료시키므로 close는
  기존 스레드 join과 control socket 해제만 수행한다. 기존 waiter·registry 정리는 유지한다.
- [ContextContractTest.java:26](../../../bindings/java/src/test/java/systems/zlink/contract/ContextContractTest.java#L26):
  ROUTER/DEALER request를 실제 수신한 뒤 socket들을 닫고, shutdown 0회·1회·2회 각각에서
  close와 반복 close가 성공하는 공개 API 회귀 테스트를 추가했다.

shutdown 여부를 별도 상태로 보관해 send를 선택하는 대안은 lifecycle 사실과 종료 규칙을
중복시킨다. Context의 기존 shutdown을 종료 원인 하나로 사용하는 대안을 선택했다.
새 wake 기구, caller의 예외 무시, 재시도 또는 timeout 증가는 없다.

- 소유 계층: **Java binding Context lifecycle**. D-B111의 Context당 completion pump 한 개 구조를 유지한다.
- Spec 조항: `bindings/doc/spec/java/README.ko.md:548–564`의 Runtime Implementation Requirements(native handle lifecycle·idempotent cleanup), `bindings/java/src/main/java/systems/zlink/contracts/core/Context.java:48–51`의 shutdown 계약, `bindings/doc/spec/async-execution-model.ko.md:119–120`의 context termination waiter·registry 정리.
- 교차언어 대조: .NET `bindings/dotnet/src/Zlink/Runtime/Handles/Context.cs:153–165`는 Dispose에서 shutdown → term을 수행한다. Node `bindings/node/src/zlink/runtime/core/context.ts:135–139,166–171`은 shutdown과 term에 위임하고, `bindings/node/native/src/addon_core.cc:1441–1471`이 native 호출을 소유한다. 두 Context teardown에는 종료 후 control send가 없다. Java의 Context pump 도입에서 생긴 구조적 결함으로, 다른 언어 변경은 필요 없다.
- 변경 분류: **B — 기존 binding lifecycle 결함 수정**. 공개 계약 변경은 없다.
- 수정 전/후 규칙 수: **종료 규칙 2 → 1**. 이전의 native shutdown과 pump close control send를, **shutdown이 종료하고 close가 해제한다**는 한 규칙으로 통합했다.

## 검증

모든 JVM 실행은 `flock -w7200 /tmp/zlink-jvm-gate.lock` 아래에서 수행했고,
`TMPDIR=/dev/shm/zlink-tmp-java`, `ZLINK_LIBRARY_PATH` unset을 적용했다.
Core prefix는 기존 설치본 `/home/hep7/.cache/zlink/core/0.17.0/linux-x64`를 사용했다.
Core 또는 local package 재빌드는 수행하지 않았다.

```bash
cd bindings/java
env -u ZLINK_LIBRARY_PATH TMPDIR=/dev/shm/zlink-tmp-java \
  ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core/0.17.0/linux-x64 \
  flock -w7200 /tmp/zlink-jvm-gate.lock ./gradlew --no-daemon test
```

| 검증 | 결과 | 증거(`/tmp/java-bingo-fix-evidence/`) |
|---|---|---|
| 수정 전 새 회귀 테스트 3개 | close 단독 성공, shutdown 1회·2회 후 close 실패 | `binding-context-regression-before.log`, `.xml` |
| 수정 후 Context 테스트 | 4개 통과, 실패·skip 없음 | `binding-context-regression-after.xml` |
| 지정된 Gradle `test` | exit 0, BUILD SUCCESSFUL, 18초 | `binding-gradle-test.log` |
| 새 binding class로 공개 API 재현: shutdown → close | request 실제 수신, shutdown·close 완료, exit 0 | `binding-repro-shutdown-close-after.log` |
| 새 binding class로 공개 API 재현: close 단독 | request 실제 수신, close 완료, exit 0 | `binding-repro-close-only-after.log` |
| `git diff --check` | 통과 | 작업 트리 검사 |

Gradle 결과는 Java 본체 109개(108 통과·기존 1 skip), Kotlin contract 4개,
perf-multi 10개, Netty extension 3개로 **총 126개 중 125 통과·1 skip·실패 0**이다.
이는 지정된 `test` task 결과이며, 별도 integration task나 Framework sample 실행 결과는 아니다.
집중 실행에 사용한 `test --tests systems.zlink.contract.ContextContractTest`는 본체 4개가
통과한 뒤 perf-multi에 같은 테스트명이 없어 exit 1이었다. 이후 필터 없는 지정 명령은
전체 성공했다. 해당 명령 오류의 로그도 `binding-context-regression-after.log`에 보존했다.

공개 API 재현의 classpath는 `bindings/java/build/classes/java/main`, `build/resources/main`과
Gradle cache의 Netty buffer/common 4.1.100.Final jar다. Framework 설치 package를 사용하지 않았다.
Bundled `build/resources/main/native/linux-x86_64/libzlink.so`와 기존 Core prefix library의
SHA-256은 모두 `1c7887c36dd2f1fe133fe3a39ccbfeb9ea42d4b051b302e3ecacf160e31b116d`다.

## BLOCKERS

없음. Binding 수정과 요청된 검증은 완료했다. Local package 재빌드 및 Java Bingo·ZoneWorld
재실행은 감독의 후속 작업이다. 다른 작업의 기존 변경은 유지했으며, 요청한 이 보고서 외의
수정 범위는 위 `bindings/java` 세 파일뿐이다.
