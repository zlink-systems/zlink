1. Java submit과 close 경로, 계약, 경합 구간

수정 대상은 bindings/java의 socket lifecycle이다. Core, spec, 다른 언어 binding과 실제 Framework source는 수정하지 않았다. 경로 설명에서 “변경 전”이라고 표시한 줄 번호는 main의 수정 전 source를 기준으로 한다.

bindings/doc/spec/README.ko.md:1851은 “socket 이 close 된 후 `reply().submit()` 호출하면 `SubmitError(TERMINATED)`.”라고 정한다. 같은 파일 :2845의 reply 결과 표는 “socket/context 종료 | `SubmitError(TERMINATED)`”를 정한다. :2829의 “소켓 close | `RequestError(TERMINATED)`”는 이미 수락된 request의 reply completion 결과다. 새 submit의 거절과 수락된 request의 완료를 구분했다. bindings/doc/spec에서 TERMINATED, close, EBUSY를 검색했다. :1230의 EBUSY는 poller attach, :2055는 handler 계약이고 socket submit의 native handle 수명 보호를 대신하지 않는다. bindings/doc/spec/async-execution-model.ko.md:87의 BUSY도 poller destroy에 관한 조항이다.

core/doc/spec/core/socket/README.ko.md:646의 lifecycle gate 설명은 :647에서 “다른 thread에서 동일 핸들에 대해 API 호출이 진행 중이면 `errno=EBUSY`로 실패한다.”, :648에서 “close가 accepted된 뒤 새 API 진입은 `errno=ESHUTDOWN`으로 실패한다.”라고 정한다. :656은 “pointer가 유효하지 않으면 `EFAULT`, opaque value가 stale 상태이면 `ESTALE`.”라고 정한다. :1438과 :1439도 invalid pointer와 admitted API의 경합을 구분한다. 따라서 ESHUTDOWN 분류만으로 이미 해제된 pointer의 사용을 방지할 수 없다. Binding은 Core 진입 전 준비 구간의 handle 수명을 소유하고, Core는 실제 API admission과 native 실행을 소유한다.

Router send는 bindings/java/src/main/java/systems/zlink/runtime/sockets/NativeRouterSocket.java:46의 builder에서 NativeSocketRuntime.submitSend 또는 submitSendBlocking으로 전달된다. request는 같은 파일 :82, 명시적 reply는 :90, Received.reply 경로는 :53의 submitReceivedReply를 지난다. DEALER의 같은 경로는 NativeDealerSocket.java:33과 :59, PAIR send는 NativePairSocket.java:27이다. Builder의 terminal은 runtime/messaging/MessageOperations.java:98의 SendBuilder.submit, :104의 submit_sync, :169의 RequestBuilder.submit, :175의 submit_sync에서 invoker를 호출한다.

변경 전 NativeSocketRuntime.java:274, :279, :284, :291, :299는 모두 ensureOpen으로 raw handle이 NULL인지 검사한 뒤 CompletionOwner에 전달했다. SocketCore.java:261의 ensureOpen은 handle을 보호하지 않는다. 변경 전 CompletionOwner.java:102의 send, :157의 blocking send, :164의 request, :208의 blocking request, :247의 reply는 :287의 submitPartsAttempt와 :254의 withNativeCall을 공유했다. Async send의 token 생성은 :435, request pending 등록은 :411, token 생성은 :426에서도 closed를 검사했다. 이 검사들은 native call에 대한 수명 예약이 아니었다.

변경 전 withNativeCall은 CompletionOwner.java:256에서 closed를 한 번 읽고 :259에서 action을 실행했다. :299부터 인자 검사와 routing id, native message header 준비를 진행하고, :318에서 message를 native header로 복사한다. :326과 :328에서 send/sendRid, :332에서 request, :336에서 reply에 socket.handle()을 넘긴다. NativeSocketRuntime.java:607의 handle()은 field를 그대로 반환한다. Native.java:577과 :614는 send flag를 추가 검사하고 :579와 :616에서 FFM downcall을 실행한다. request와 reply의 실제 downcall은 Native.java:751과 :763이다. 즉 외부 ensureOpen 이후, token/pending 등록 이후, withNativeCall의 closed 검사 이후, message 준비 중, handle을 읽은 뒤 FFM에 진입하기 전까지 모두 close가 추월할 수 있었다. Writable retry도 CompletionOwner.java:760과 :800의 동일 submitPartsAttempt를 사용하므로 같은 문제를 갖는다.

Close는 NativeRouterSocket.java:107 또는 NativeSocketBase.java:218에서 NativeSocketRuntime.closeInternal로 들어간다. 변경 전 NativeSocketRuntime.java:615의 closeInternal은 SocketCore.java:243을 거쳐 CompletionOwner.java:358의 closeNativeSocket을 호출한다. :359의 Native.close와 Native.java:520의 downcall이 native handle을 해제한다. 성공 뒤에야 CompletionOwner.java:362에서 closed를 설정하고 NativeSocketRuntime.java:624에서 handle을 NULL로 바꾼다. 따라서 close가 Core에서 진행 중인 구간과 native close 성공 뒤 closed를 쓰기 전 구간에도 새 submit이 이전 검사를 통과할 수 있었다. close 실패 시 NativeSocketRuntime.java:619에서 예외를 던져 :624에 도달하지 않으므로 기존 handle과 owner는 유지된다.

Received.java:665, :716, :728은 IllegalStateException을 TERMINATED로 바꾼다. 이 변환은 이미 native에 전달한 released handle을 회수할 수 없다. 수정 전 load 재현에서 reply가 INVALID_HANDLE/14와 NOT_FOUND/2를 반환한 이유가 이 경계에 있다.

2. 설계와 변경

CompletionOwner.java:260의 withNativeCall이 기존 ownerLock 안에서 closed를 확인하고 activeNativeCalls를 증가시킨다. Native 인자 준비와 호출은 lock 밖에서 실행한다. 반환과 예외 모두 finally에서 같은 lock을 얻어 수명 예약을 해제한다. CompletionOwner.java:371의 closeNativeSocket은 같은 ownerLock 안에서 예약 수를 확인한다. 예약이 있으면 Java binding이 ZlinkCloseException(BUSY, EBUSY)을 반환하며 Native.close를 호출하지 않는다. 이 BUSY는 Core가 아직 관측할 수 없는 Java 준비 구간도 보호한다. Core의 BUSY를 받았다고 보고하지 않는다.

예약이 없으면 ownerLock을 유지한 채 Native.close를 호출한다. 이 동안 새 submit의 예약은 시작되지 않는다. Native close가 성공하면 기존 closed를 true로 바꾼 뒤 lock을 해제한다. 기다리던 submit은 CompletionOwner.java:386의 한 predicate에서 ZlinkSubmitException(TERMINATED, ESHUTDOWN)을 받고 socket handle을 읽지 않는다. 이미 종료한 owner의 중복 close는 native에 재진입하지 않는다. Native close가 BUSY 등으로 실패하면 closed를 바꾸지 않고 기존 closeInternal의 실패 경로를 유지한다. Close가 수락되지 않았으므로 이후 submit은 계속 가능하다.

새 field activeNativeCalls는 ownerLock으로 보호하는 정수다. 이는 Java가 native 인자를 준비하기 시작한 시점부터 native 호출이 돌아오는 시점까지 handle을 보유하는 호출 수다. Core의 admitted API 수나 pending completion 수를 복제하지 않는다. 기존 closed, ownerLock, pending, retry queue를 재사용했다. 새 queue, cache, timer, retry, lock, closing flag는 추가하지 않았다.

Native action 전체에 ownerLock을 잡는 대안은 막힌 blocking send가 다른 submit과 completion 진행을 막으므로 채택하지 않았다. 별도 read/write lock을 추가하는 대안은 이미 ownerLock이 소유한 lifecycle 상태에 동기화 수단을 더하므로 채택하지 않았다. 기존 lock의 짧은 예약과 fail-fast BUSY는 동시 submit을 유지한다.

Canonical send/request/reply submit의 종료 여부를 독립적으로 판정하는 predicate 위치는 수정 전 5곳에서 수정 후 1곳으로 줄었다. 수정 전 위치는 SocketCore.java:261, CompletionOwner.java:256, :415, :428, :437이다. 수정 후 위치는 CompletionOwner.java:386이다. NativeSocketRuntime의 5개 outer ensureOpen 호출을 삭제했고 CompletionOwner의 4개 predicate를 이 helper로 합쳤다. 공개 poller owner의 부재와 lifecycle 종료가 경합할 때도 :727의 ensurePublicOwner가 같은 predicate로 종료를 먼저 판단한다. 호출 위치 수를 결정 위치 수로 중복 계산하지 않았다.

소유 계층은 Java binding의 CompletionOwner다.

Spec 근거는 binding README :1851과 :2845, Core socket README §2와 :646-658이다.

교차언어 정적 대조 결과는 .NET과 C++에 같은 수명 공백이 있고 Node의 일반 동기 public 호출에는 동일한 thread 경합 경로가 없다는 것이다.

변경 분류는 B, 기존 계약 위반의 수정이다.

리팩토링 점검에서 종료 predicate 중복 4곳과 outer 검사 handoff 5곳을 제거했다. Received.java:665, :716, :728의 기존 예외 변환은 유지했다. 이 코드는 다른 invoker의 호환 오류 변환이며 handle 보유를 결정하지 않는다. Pending 등록과 native admission 전 검사는 같은 helper를 호출하지만 등록 및 준비 사이에 close가 수락될 수 있어 두 진입 검사가 필요하다. Unused helper나 새 queue·lock은 추가하지 않았다. Canonical submit의 종료 predicate 중복 4곳과 outer handoff 5곳을 줄였고 Received 변환 3곳은 유지했다. CompletionOwner.java:569와 :592의 completion 조회 전 closed 검사도 점검했다. 실제 completionRecv는 :577과 :597에서 실행되며 이번 submit 예약의 대상은 아니다. 이 별도 조회 경로와 raw recv·option·bind의 close 경합을 수정했다고 판정하지 않는다.

변경한 test는 SubmitCloseAdmissionContractTest.java, SubmitCloseRaceContractTest.java, RequestRetryCloseContractTest.java와 공통 CompletionNativeFixture.java다. 첫 test는 Core에 진입하기 전의 중단 지점, 실패한 action의 예약 해제, Core 자체가 BUSY를 반환한 뒤의 재사용, 닫힌 socket의 send/request/reply와 sync terminal 및 메시지 보존을 확인한다. 기존 ConcurrentAdmissionContractTest는 blocked send 중 다른 submit과 completion drain의 진행을 계속 확인한다. RequestRetryCloseContractTest는 진행 중인 retry에 close가 성공하던 기존 scripted 상황을 실제 Core 계약에 맞게 바꿨다. 진행 중에는 BUSY와 미완료 drain을 확인하고, retry가 수락되어 돌아온 뒤 close하여 reply future가 RequestResult.TERMINATED로 끝나는지 확인한다. 허용 submit 결과를 늘리지 않았다. CompletionNativeFixture.java:201은 enum 반환값을 먼저 준비한 뒤 upcall의 errno를 설정하게 했다. 새 BUSY 검증이 실제로 발견한 fixture 초기화 순서 결함이며 errno assertion을 유지했다.

3. 검증

이 절의 상대 로그 경로는 /root/jobs/fx/cx/v21/s033/bclose/를 기준으로 한다. 환경은 Linux x64와 OpenJDK 25.0.4.1이다. Cached Core 1.23.0의 /root/.cache/zlink/core/1.23.0/linux-x64/lib/libzlink.so를 ZLINK_LIBRARY_PATH로 명시했다. ZLINK_CORE_PACKAGE_PREFIX는 같은 linux-x64 package를 가리켰다. Core library SHA-256은 6f496304ef6e1287f57234567231bbd2b76ad2392f99e816001417d01215bf01이다. 변경 전후 CompletionOwner.class의 hash도 서로 다름을 확인했고 native-and-binding-sha256.txt에 기록했다.

부하는 `/root/jobs/fx/cx/measure-shared.sh stress-ng --cpu 16 --cpu-method loop --metrics-brief --log-file /root/jobs/fx/cx/v21/s033/bclose/load.log`로 실행했다. 아래 재현, Java race, Framework race와 최종 전체 suite가 이 부하 중에 실행됐다. 부하는 PID 3267178에 SIGTERM을 보내 종료했다. 총 17분 4.90초 실행했고 stressor 16개가 종료됐다. 5초 간격 memory.log의 MemAvailable 최저값은 13,721,300 kB, 13.086 GiB였다. 12 GiB 미만을 관측하지 않았다.

수정 전 재현은 bclose/baseline-bindings에 복사한 main source에서 수행했다. Core 진입 직전에 action을 멈추는 SubmitCloseAdmissionContractTest는 1회 실행해 실패했다. 진행 중인 submit을 두고 close가 성공해 BUSY 예외가 없었다. 로그는 admission-before.log, JUnit은 results/admission-before/이다. 변경 전 source를 분리해 두고 같은 검사가 기존 결함을 실제로 검출하는지 확인했다.

수정 전 부하 race는 `./gradlew --no-daemon --max-workers=2 :test --tests '*SubmitCloseRaceContractTest'`로 각 연산 1,000회 실행했다. 매 회 동일 handle에 submit thread 4개와 close thread 1개를 동시에 시작했다. 연산마다 총 4,000개 submit이다. Send는 OK 3,165개, TERMINATED/108 52개, INVALID_HANDLE/14 39개, IllegalStateException 744개였다. Request는 OK 3,306개, TERMINATED/108 39개, IllegalStateException 655개였다. Reply는 OK 2,937개, TERMINATED/108 62개, TERMINATED/0 969개, INVALID_HANDLE/14 16개, NOT_FOUND/2 16개였다. 세 race test 모두 의도한 결과 제한 위반을 검출했다. 로그는 races-before-1000.log와 races-before-outcomes.txt, JUnit은 results/races-before-1000/이다.

수정 뒤 첫 focused 검증은 races-after-1000.log에 있다. SubmitCloseAdmissionContractTest 3개, ConcurrentAdmissionContractTest 1개, RequestRetryCloseContractTest 1개, race test 3개가 통과했다. JUnit은 results/races-after-1000/이다. 초기 admission-after.log 명령은 task를 :test로 한정하지 않아 하위 perf 프로젝트의 “No tests found”로 Gradle이 실패했다. 그 명령의 기본 binding test 5개는 통과했으며 이후 :test로 한정한 위 명령은 성공했다.

최종 source의 race는 전체 suite에서도 각 1,000회 다시 실행했다. Send 4,000개는 OK 3,011개와 TERMINATED/108 989개였다. Request 4,000개는 OK 2,960개와 TERMINATED/108 1,040개였다. Reply 4,000개는 OK 2,892개와 TERMINATED/108 1,108개였다. INVALID_HANDLE, NOT_FOUND, 다른 submit 결과와 IllegalStateException은 모두 0개였다. Send와 request는 반환 snapshot이 실제로 OK인지도 검사하므로 BACKPRESSURED를 성공으로 계산하지 않는다. Close 결과는 send에서 OK 917회와 BUSY 83회, request에서 OK 975회와 BUSY 25회, reply에서 OK 919회와 BUSY 81회였다. BUSY를 받은 회차는 submit을 다시 제출하지 않고 모든 submit이 돌아온 뒤 resource cleanup에서 socket을 닫았다. 이 결과는 binding-full.log와 results/binding-full/build/test-results/test/TEST-systems.zlink.runtime.sockets.SubmitCloseRaceContractTest.xml에 있다.

PR #4056 검증은 framework-scratch에서 수행했다. Main과 jdr6 diff의 기반 source가 달라 /root/worktree/zlink-jdr의 Java source를 scratch로 복사했다. 대상 세 파일의 현재 git diff를 scratch에 역적용해 tracked 기반을 복원한 뒤 /root/jobs/fx/cx/v21/s033/jdr6/change.diff를 적용했다. 원본 worktree는 수정하지 않았다. 적용 로그는 framework-patch.log다. Scratch test의 두 @RepeatedTest(100)을 각각 1,000으로 바꾸고 결과 집계와 loaded binding 위치 출력만 추가했다. 원래 assertion과 허용 결과는 유지했다. 이 검증용 변경은 framework-measurement.diff, 실제 사용한 test source는 framework-fixture-used.java.txt에 보존했다.

Framework 명령은 framework-scratch/framework/languages/java에서 `./gradlew --no-daemon --max-workers=2 -Pzlink.includeSamples=false :zlink-framework-core:test --tests '*ZLinkJavaRawServicePortContractTest.repliesRacing*'`였다. ZLINK_JAVA_BINDINGS_SOURCE=/root/worktree/zlink-4060-binding-close/bindings/java로 기존 dependency substitution을 사용했다. 실제 로드 위치는 /root/worktree/zlink-4060-binding-close/bindings/java/build/libs/zlink-java-1.23.0.jar였다. 로그는 framework-races-1000.log, JUnit은 results/framework-races-1000/이다.

Framework port close는 1,000/1,000회 통과했다. Reply 8,000개는 OK 3,296개, Framework의 IllegalStateException("service port is closed") 4,702개, binding의 TERMINATED/108 2개였다. Runtime close도 1,000/1,000회 통과했다. Reply 8,000개는 OK 7,373개와 Framework의 같은 종료 거절 627개였다. 두 경우 모두 close 성공, INVALID_HANDLE 0개, NOT_FOUND 0개, binding의 IllegalStateException("socket is closed") 0개였다. JUnit test 시간은 합계 73.498초였고 처음부터 source를 컴파일한 Gradle 전체 시간은 4분 56초였다.

새 Core BUSY 검증은 첫 전체 실행에서 errno 16 대신 2를 관측해 실패했다. 이는 CompletionNativeFixture가 Java enum을 처음 초기화하기 전에 libc errno를 설정하던 순서 때문이었다. Native.java:1167의 실제 errno는 FFM downcall 종료 시 캡처되므로 production의 deferred errno 읽기 문제로 바꾸지 않았다. Fixture에서 enum 반환값을 먼저 준비하고 errno를 마지막에 설정하도록 수정했다. 기대한 BUSY/EBUSY assertion은 유지했다. 첫 실행 로그와 XML은 binding-full-first.log와 results/binding-full-first/에 보존했다. 수정 뒤 같은 test가 native-busy-after.log와 results/native-busy-after/에서 통과했다.

최종 전체 명령은 bindings/java에서 `./gradlew --no-daemon --max-workers=1 --continue test integrationTest`였다. 이 명령도 measure-shared.sh와 위 Core 환경 변수를 사용했다. 기본 Java suite는 172개 중 170개 통과와 기존 skip 2개, integration은 36개 통과, Netty는 3개 통과, Kotlin contract는 4개 통과, perf-multi는 23개 통과였다. 합계 238개 중 236개 통과, 2개 skip, 실패와 error는 0개다. Samples와 perf-single의 test task는 NO-SOURCE였고 해당 source 컴파일은 성공했다. 전체 시간은 2분 12초였다. 로그는 binding-full.log, 합계는 binding-full-summary.json, 각 JUnit은 results/binding-full/에 보존했다.

기존 skip은 LibraryLoaderTest.coreWithoutSnapshotExportIsRejectedBeforeContextCreation과 MonitorConnectionIdentityContractTest.inprocClosedKeepsReadyConnectionIdentity다. 전자는 ZLINK_TEST_OLD_CORE_LIBRARY 환경 변수로 제공하는 구버전 Core가 없으면 실행하지 않는 test다. 후자는 원래 @Disabled("spec gap: inproc peer close emits no CLOSED — D-B102")였다. 이번 작업에서 skip이나 assertion을 완화하지 않았다.

`scripts/format/format.sh --check java`는 통과했다. 로그는 format-java.log다. 이 script의 :52 source_roots는 framework/languages/java만 선택하며 bindings/java를 검사하지 않는다. 별도 binding 전체 formatter task는 찾지 못했다. 변경한 SubmitCloseAdmissionContractTest, SubmitCloseRaceContractTest와 RequestRetryCloseContractTest는 pinned google-java-format 1.36.1의 AOSP 모드로 정리하고 --dry-run --set-exit-if-changed를 통과했다. 로그는 format-binding-tests.log다. 기존 binding runtime 파일 전체를 다시 format하지 않았다.

최종 git diff --check는 통과했고 로그는 diff-check.log다. 신규 untracked test 두 파일도 --no-index --check로 공백 오류가 없음을 확인했으며 new-files-whitespace-check.log에 기록했다. change.diff에는 tracked 수정과 신규 test를 모두 포함했다. 변경 전 source에 대한 patch --dry-run도 통과했고 로그는 patch-check.log다. 최종 변경 파일은 CompletionOwner.java, NativeSocketRuntime.java, CompletionNativeFixture.java, RequestRetryCloseContractTest.java, SubmitCloseAdmissionContractTest.java, SubmitCloseRaceContractTest.java다. 남은 실행 실패는 없다. 로그와 JUnit을 보존한 뒤 임시 baseline과 Framework scratch source는 삭제했다.

4. 다른 언어 binding 점검

.NET에는 같은 공백이 있다. bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:15와 :28은 raw IntPtr을 owner에 저장한다. :223의 EnsureOpenForSubmit 다음 :226에서 handle을 복사하고 :243에서 zlink_reply를 호출한다. :406의 volatile 종료 검사는 이를 close와 정렬하지 않는다. Send는 :37 또는 :113의 검사 뒤 :441에서 handle을 복사하고 :475 또는 :478에서 Core에 진입한다. Request는 :128 또는 :188의 검사 뒤 :491에서 복사하고 :523에서 Core에 진입한다. Runtime/Sockets/SocketHandle.cs:39의 zlink_close가 handle을 해제하고 :42에서 field를 0으로 바꾼다. SocketKernel.Lifecycle.cs:44의 CompleteClose는 그 뒤에 실행되며 CompletionOwner.cs:361에서 _closing을 설정한다. SocketHandle은 SafeHandle 참조 예약을 제공하는 타입이 아니고 단순 IDisposable이다. Owner가 보관한 IntPtr도 field 초기화 이후 계속 남는다. 여러 파일의 raw handle 소유와 close를 함께 바꿔야 하므로 Java 수정의 단순 복사로 보지 않았고 수정하지 않았다.

C++에도 같은 공백이 있다. bindings/cpp/src/Runtime/Sockets/router.cpp:26, :89, :103은 send/request/reply builder에 raw socket pointer를 저장한다. Runtime/Messaging/operation_state.hpp:116과 :125의 weak_ptr/lock은 runtime 객체의 수명만 보장하며 native handle close와 정렬되지 않는다. Send는 operation_submit.hpp:59와 :64, request는 :165, reply는 reply_operations.cpp:34에서 캡처한 pointer로 Core에 진입한다. Runtime/Native/socket_handle.hpp:59의 valid 검사와 :74의 zlink_close 사이에는 API 예약이 없다. 성공 뒤 :76에서 atomic _open을 false로 바꾸지만 :81의 handle()은 저장한 raw pointer를 그대로 반환한다. Runtime/Sockets/socket.cpp:86의 close도 :90에서 native close를 먼저 실행한 뒤 :95에서 completion owner를 shutdown한다. Builder가 오래 보유한 pointer까지 함께 보호해야 하므로 이번 작업에서 수정하지 않았다.

Node는 일반 public socket 호출이 같은 Node environment의 event loop에서 동기로 실행된다. bindings/node/src/zlink/runtime/messaging/completion_owner.ts:288과 :387의 submit에서 native addon 호출까지 await나 worker handoff가 없다. runtime/sockets/socket_base.ts:143의 close도 :151의 native close가 돌아온 뒤 :157에서 owner를 해제하고 :158에서 _native를 NULL로 바꾼다. Native addon의 socketSubmitSend는 native/src/addon_core.cc:2013에서 external pointer를 읽어 :2034에서 동기로 Core에 진입한다. Socket close도 :1709의 동기 호출이다. 같은 파일 :32의 environment별 socket 집합과 :1698의 N-API external을 사용하며 공유하는 객체는 context다. Java와 같은 두 public thread의 동일 socket submit/close 경로는 정적 검토에서 확인되지 않았다.

다만 Node의 close 후 거절은 Java의 이번 계약과 차이가 있다. runtime/handles/native_handle.ts:5는 closed handle에 NULL을 반환하고, completion_owner.ts:245의 handle getter는 주석과 달리 throw하지 않는다. :831의 completionOwnerOf는 owner가 제거된 뒤 TypeError를 낸다. router_socket.ts:100의 replyDirect는 :108에서 얻은 handle을 :107의 socketReply에 전달한다. 따라서 닫힌 socket의 submit 오류를 모두 TERMINATED로 투영한다고 판정할 수 없다. 이 결과는 일반 동기 호출의 thread 경합 유무와 구분했다. Payload getter 등의 재진입까지 검증한 결과는 아니며 Node source는 수정하지 않았다.
