1. 기존 잠금 구간에서 예약하고 원자적으로 해제하도록 수정했다.

작업 대상은 /root/worktree/zlink-4060-binding-close의 기존 미커밋 Java binding 수정이다. report-run1.md와 change-run1.diff를 먼저 읽었다. 현재 worktree는 detached HEAD이며 그대로 수정했다. 아래 CompletionOwner.java, SocketSendPlane.java, SocketCore.java, NativeSocketRuntime.java는 bindings/java/src/main/java/systems/zlink/runtime/sockets/ 아래 파일이다. main 줄 번호는 change-run1.diff의 기반 blob bb6ce139b2를 복원한 main-CompletionOwner.java.txt에 해당한다. 그 외 줄 번호는 최종 source 기준이다. 로그와 결과의 상대 경로는 이 보고서가 있는 bclose/ 기준이다.

CompletionOwner.java:101의 기존 activeNativeCalls를 int에서 AtomicInteger로 대체했다. 증가 네 곳은 :279, :441, :450, :463이다. 모두 ownerLock 안에서 :392의 ensureOpenForSubmitLocked를 통과한 뒤 실행한다. 감소는 같은 AtomicInteger의 decrementAndGet이며 ownerLock을 다시 잡지 않는다. 토큰과 pending을 만드는 경로는 기존 잠금 구간을 재사용한다. 새 lock, flag, queue, timer는 없다.

closeNativeSocket은 CompletionOwner.java:378에서 ownerLock을 잡고 :382에서 예약 수를 읽는다. 0이 아니면 :383에서 BUSY/EBUSY를 반환하는 예외를 던진다. 0이면 잠금을 유지한 채 :384에서 Native.close를 호출하고, 성공한 경우에만 :386에서 closed를 설정한다. Native.close 실패는 closed를 바꾸지 않는다. 모든 증가는 이 잠금 안에서만 가능하므로 close가 진행 중인 예약을 누락할 수 없다. 감소도 원자적 연산이므로 동시 증가와 값을 잃지 않는다. 이미 끝난 호출을 0이 아닌 값으로 관측하는 경우의 결과는 호출자가 다시 시도할 BUSY다.

네 예약 경로의 종료 검사는 CompletionOwner.java:392의 동일 helper를 사용한다. close가 먼저 성공하면 이 helper가 TERMINATED/ESHUTDOWN을 던져 native handle 사용 전에 거절한다. 반대로 예약이 먼저 성공하면 해당 native 호출과 인자 준비가 끝날 때까지 close가 BUSY다. Native 호출 동안 ownerLock은 잡지 않는다.

DONT_WAIT trackNoWaitSend는 CompletionOwner.java:294에서 nextContextToken으로 들어가 :447의 기존 잠금 안에서 closed 확인과 토큰 할당을 마치고 :450에서 예약한다. :295의 try-with-resources와 :308의 finally가 Arena 생성·할당, context 변환, submitter 호출, 결과 변환, Arena 종료를 모두 포함하며 :309에서 해제한다. submitter의 null 검사는 :293에서 예약 전에 끝난다.

비동기 send는 CompletionOwner.java:111에서 nextSendContextToken을 호출한다. :456의 기존 잠금 안에서 closed 확인, 배열 확장과 unpublished 토큰 기록을 끝내고 :463에서 예약한다. 즉 배열 할당 실패는 예약 전에 발생한다. :115의 try가 context 변환과 :116의 submitPartsAttempt 호출을 포함하고 :120의 finally에서 해제한다. 원래 메시지 정리나 backpressure 상태 등록 전에 해제하며, 기존 :140의 실패 정리는 그대로 수행한다.

비동기 request는 CompletionOwner.java:179에서 register를 호출한다. :435의 기존 잠금에서 closed 확인, Pending 생성과 map 등록을 끝낸 뒤 :441에서 예약한다. Pending 생성이나 map 등록 중 실패는 예약 전이다. :184의 try가 state.context와 :185의 submitPartsAttempt를 포함하고 :189의 finally에서 해제한다. timeout 변환은 :178에서 예약 전에 수행한다. 이후 실패 처리는 :206의 state.reject가 담당한다.

동기 request는 CompletionOwner.java:243에서 같은 register를 호출해 :441에서 예약한다. :245의 try가 :247의 timeoutMillis와 state.context 평가까지 포함하고 :250의 finally에서 해제한다. 따라서 timeout 변환이 submitPartsAttempt 진입 전에 예외를 던져도 해제한다. :259의 catch는 pending을 거절한다. 예약은 reply future를 기다리는 :229의 drainInline 전에 해제된다.

기존 ownerLock 구간이 없는 호출은 CompletionOwner.java:275의 withNativeCall을 사용한다. action null 검사는 :276에서 예약 전에 끝난다. :277에서 잠금을 한 번 잡아 :279에서 예약하고 :281의 try와 :284의 finally로 정상 반환, RuntimeException, Error 모두에서 해제한다. action lambda를 만드는 작업은 이 예약 전에 수행한다. 동기 send :168, reply :268, writable send retry :783, writable request retry :824가 이 경로다. 각 lambda 안의 state.context와 native 준비도 예약의 보호를 받는다.

SocketSendPlane.java:40, :62, :85, :127, :173, :184, :212의 기존 blocking 호출도 같은 withNativeCall의 :279와 :284를 사용한다. 해당 파일 :118, :308, :332의 DONT_WAIT handoff는 trackNoWaitSend의 :450 예약과 :309 해제를 사용한다. SocketSendPlane은 수정하지 않았다.

공통 native 준비 함수는 CompletionOwner.java:313 하나다. :318의 validateParts, :319의 scratch 초기화, :323의 routing id 쓰기, :333의 header 공간 확보, :338의 message copy와 :345, :347, :351, :355의 native 호출은 모두 위 호출자들의 try/finally 안에 있다. 이 함수가 예약을 다시 잡는 wrapper를 제거했다.

예약 후 다시 withNativeCall에서 검사하는 첫 수정의 구조와, 기존 토큰·pending 구간에 예약을 합치는 구조를 비교했다. 후자는 잠금 진입을 추가하지 않으면서 동일한 close 순서를 보장하므로 채택했다. native 실행 전체를 ownerLock으로 감싸면 blocking 호출이 다른 submit과 completion drain을 막으므로 채택하지 않았다.

소유 계층은 Java binding의 CompletionOwner이며 Core는 실제 native API admission을 소유한다.
Spec 근거는 bindings/doc/spec/README.ko.md:1851과 :2845의 TERMINATED 계약, core/doc/spec/core/socket/README.ko.md:646의 lifecycle gate와 :647의 BUSY, :648의 ESHUTDOWN 계약이다.
교차언어 대조는 report-run1.md의 정적 조사 결과를 유지한다. .NET과 C++에는 같은 native handle 수명 공백이 있고 Node의 일반 동기 호출에는 Java와 같은 두 public thread 경합 경로가 확인되지 않았다. 다른 binding은 수정하지 않았다.
변경 분류는 B이며 기존 계약 위반 수정의 예약 비용을 줄인 변경이다.

종료 결과의 판정 위치 수는 첫 수정 전후를 포함하면 main 5곳에서 1곳으로 줄었고, 이번 수정 전후는 1곳에서 1곳이다. 최종 위치는 CompletionOwner.java:392다. 예약과 native close의 충돌 판정도 첫 수정과 이번 수정 모두 :382의 한 곳이다. 기존 정수 상태를 원자적 정수로 대체했으며 별도 상태를 추가하지 않았다.

리팩토링 점검에서 토큰·pending 예약 뒤 다시 예약하던 네 경로인 trackNoWaitSend, 비동기 send, 비동기 request, 동기 request의 중복 handoff를 제거했다. 첫 수정 CompletionOwner.java:282와 :304의 withNativeCall 재진입은 최종 :295, :116, :185, :246의 직접 실행으로 바뀌었다. 첫 수정의 :269 잠금 재진입을 통한 해제도 최종 :284의 원자적 감소로 바꿨다.

불필요 helper 세 개를 제거했다. main CompletionOwner.java:402의 단일 호출 registerRequest wrapper와 :406의 미사용 register overload를 제거해 최종 :243에서 기존 register를 직접 쓴다. main :287의 submitPartsAttempt와 :294의 submitPartsAttemptLocked 두 단계는 최종 :313 한 함수로 합쳤다.

남겨 둔 검사와 handoff도 확인했다. CompletionOwner.java:730의 public owner 검사 전 종료 확인과 :436의 pending 등록 전 확인은 같은 종료 predicate를 호출한다. 두 잠금 사이에 close가 가능하므로 등록 경계에서 다시 확인한다. :602의 retry 처리 전 closed 확인과 :1179의 beginRetry, :783과 :824의 native 예약은 각각 drain 진행, pending 취소·정리, handle 수명에 관한 기존 경계다. 새 종료 predicate를 추가하지 않았다.

SocketSendPlane.java:206과 :219의 기존 ensureOpen 및 CompletionOwner.java:392의 검사, Received.java:665, :716, :728의 IllegalStateException 변환은 유지했다. 전자는 기존 raw send 진입 검사이고 후자는 다른 invoker도 받는 호환 변환이다. 이번 수정은 새 예약 상태를 이곳에 복제하지 않았다. CompletionOwner.java:570과 :593의 completion 조회 종료 검사도 유지했으며 별도 completionRecv 대 close 경합을 해결했다고 판정하지 않았다.

점검 결과는 중복 예약 handoff 네 경로와 불필요 helper 세 개 제거다. 나머지 기존 검사와 변환은 위 위치에 기록했다. 별도 이슈나 스펙 변경은 없다.

2. ownerLock 진입 횟수는 기존 잠금이 있는 경로에서 main과 같다.

아래 횟수는 submit이 직접 수행하는 ownerLock 진입을 세며 사용자 callback, 다른 thread의 completion drain, resource cleanup의 close는 제외한다. main은 main-CompletionOwner.java.txt의 줄 번호다. first pass가 추가한 예약과 해제 잠금은 각각 한 번이었다. 이번에는 제출당 원자 증가·감소 각 한 번이 남는다. 시간 benchmark는 실행하지 않았다.

Framework의 DONT_WAIT send 경로인 trackNoWaitSend는 main 1회, 첫 수정 3회, 최종 1회다. main :268에서 :427의 nextContextToken 잠금을 거쳤고 최종은 :294에서 :447만 거친다. 최종 :309의 해제에는 잠금이 없다. SocketSendPlane의 단일 frame, routing id, copied payload, multipart DONT_WAIT 제출은 모두 이 경로를 공유한다.

비동기 send의 native 호출까지는 main 2회, 최종 2회다. main :103에서 :703의 hasPublicOwner와 :104에서 :436의 nextSendContextToken을 거친다. 최종 :110에서 :723과 :111에서 :456으로 대응한다. OK 반환까지는 main 3회, 첫 수정 5회, 최종 3회다. main :117에서 :449의 discard, 최종 :127에서 :469의 discard가 한 번 추가된다.

비동기 send의 BACKPRESSURED 반환도 main 3회, 최종 3회다. 위 두 진입 뒤 main :135, 최종 :145의 pending 등록 잠금이 한 번 추가된다. native 준비·호출 실패는 discard까지 main과 최종 모두 3회다. OK 뒤 closeParts가 실패해 catch에서 discard를 다시 하는 기존 경로는 둘 다 4회이며 이번 변경에서 늘리지 않았다.

비동기 request는 main 2회, 첫 수정 4회, 최종 2회다. main :166의 ensurePublicOwner는 :703의 hasPublicOwner 잠금을 사용했고 :168의 register는 :414에서 잠갔다. 최종 :177은 :729에서 한 번, :179는 :435에서 한 번 잠근다. OK와 BACKPRESSURED 및 native 준비 실패 모두 이 두 번이며 :189의 해제에는 잠금이 없다.

동기 request의 최초 native 제출은 main 1회, 첫 수정 3회, 최종 1회다. main :228의 registerRequest가 :414의 register 잠금을 거쳤고 최종 :243은 :435로 바로 들어간다. 동기 reply 대기에는 기존 drainInline의 hasPublicOwner 검사도 있으므로 reply 완료까지는 main과 최종 모두 이 한 번 및 completion 처리별 잠금이 추가된다. 해당 검사는 main :524와 최종 :544, 실제 잠금은 main :703과 최종 :723이다. completion capture의 main :614, :638과 최종 :634, :658도 그대로다. 따라서 전체 동기 request를 무조건 1회라고 계산하지 않았다.

동기 send와 reply의 직접 native 제출은 main 0회, 첫 수정 2회, 최종 1회다. main :157과 :247에서 :254의 잠금 없는 withNativeCall을 썼다. 최종 :168과 :268은 :277에서 한 번만 잠근다. 원래 잠금 구간이 없는 경로에 예약 잠금 1회를 허용한다는 요구에 해당한다.

writable send와 request 재제출 시도도 main 0회, 첫 수정 2회, 최종 1회다. main :760과 :800의 native 시도는 잠금 없는 withNativeCall로 들어갔다. 최종 :783과 :824는 :277에서 한 번 잠근다. 재제출하지 않고 beginRetry에서 반환하는 경우에는 모두 0회다. completion drain 자체의 잠금은 이 native 재제출 횟수와 구분했다.

SocketSendPlane의 위 일곱 withNativeCall 호출도 main 0회에서 최종 1회가 된다. 기존 잠금이 없는 경로의 예약 진입이며 해제 잠금은 없다. 해당 파일 자체의 잠금 횟수나 분기는 추가하지 않았다.

3. 지정한 부하에서 admission, binding 전체와 두 종류 경합이 모두 통과했다.

모든 build와 test는 /root/jobs/fx/cx/measure-shared.sh를 거쳤다. Java native 환경은 ZLINK_LIBRARY_PATH=/root/.cache/zlink/core/1.23.0/linux-x64/lib/libzlink.so, ZLINK_CORE_PACKAGE_PREFIX=/root/.cache/zlink/core/1.23.0/linux-x64였다. Core SHA-256은 6f496304ef6e1287f57234567231bbd2b76ad2392f99e816001417d01215bf01이다. native-and-binding-run2-sha256.json에 Core, 수정한 source, compiled class와 jar의 hash를 기록했다. jar 안의 CompletionOwner.class가 이 worktree의 compiled class와 같은 것도 확인했다.

부하는 measure-shared.sh python3 bclose/run2-load.py로 실행했다. 이 runner가 stress-ng --cpu 16 --cpu-method loop --metrics-brief --log-file bclose/load-run2.log를 실행하고 MemAvailable을 5초 간격으로 기록했다. 아래 모든 성공한 검증은 이 부하 중 실행됐다. 부하 runner PID 3422809에 SIGTERM을 보내 stress-ng PID 3422810으로 전달했고 16개 CPU stressor가 모두 종료됐다. 실행 시간은 8분 36.04초였다. memory-run2.log의 104개 표본 중 최저 MemAvailable은 13,624,652 kB, 12.99348 GiB였다.

최소 검증 명령은 bindings/java에서 ./gradlew --no-daemon --max-workers=2 :test --tests '*SubmitCloseAdmissionContractTest' --tests '*ConcurrentAdmissionContractTest' --tests '*RequestRetryCloseContractTest'였다. admission-run2.log와 results/admission-run2/에 결과를 보존했다. admission 5개, concurrent admission 1개, request retry close 1개가 통과했다.

새 admission 검사는 withNativeCall과 trackNoWaitSend의 실패 action 뒤 close 성공을 확인한다. 비동기 send, 동기 send, 비동기 request, 동기 request, reply에는 빈 parts를 넣어 native 준비 오류를 의도적으로 일으킨 뒤 close 성공을 확인한다. 동기 request에는 int 범위를 넘는 timeout도 넣어 submitPartsAttempt 호출 전 예외에서 예약이 해제되는지 확인한다. 정상 action 진행 중 close는 BUSY/EBUSY이고, close 뒤 submit은 TERMINATED/ESHUTDOWN이며 메시지를 소비하지 않는 기존 검사도 유지했다. Core 자체가 BUSY를 반환한 뒤 socket을 재사용하는 검사도 통과했다. 첫 수정의 실패 재현 근거인 admission-before.log와 races-before-1000.log는 그대로 보존했다.

binding 경합 명령은 bindings/java에서 ./gradlew --no-daemon --max-workers=2 :test --tests '*SubmitCloseRaceContractTest'였다. send, request, reply를 각각 1,000회 실행하고 매 회 submit thread 4개와 close thread 1개를 경합시켰다. binding-races-run2-1000.log와 results/binding-races-run2-1000/에 결과가 있다. send 4,000개는 OK 2,969개와 TERMINATED/108 1,031개였다. request 4,000개는 OK 3,122개와 TERMINATED/108 878개였다. reply 4,000개는 OK 2,843개와 TERMINATED/108 1,157개였다. close의 BUSY/OK는 각각 send 53/947, request 75/925, reply 64/936이었다. 다른 submit 결과는 0개였다.

전체 binding 명령은 bindings/java에서 ./gradlew --no-daemon --max-workers=2 --continue test integrationTest였다. binding-full-run2.log와 results/binding-full-run2/, binding-full-run2-summary.json에 결과를 보존했다. root test 173개, integration 36개, Netty 3개, Kotlin contract 4개, perf-multi 23개로 합계 239개였다. 237개 통과, 기존 skip 2개, failure와 error는 0개였다. 전체 Gradle 실행 시간은 1분 59초였다. 기존 skip은 LibraryLoaderTest.coreWithoutSnapshotExportIsRejectedBeforeContextCreation과 MonitorConnectionIdentityContractTest.inprocClosedKeepsReadyConnectionIdentity다.

전체 suite에서도 세 binding 경합을 각각 1,000회 실행했다. send 4,000개는 OK 3,317개와 TERMINATED/108 683개, request는 OK 3,622개와 TERMINATED/108 378개, reply는 OK 3,074개와 TERMINATED/108 926개였다. close의 BUSY/OK는 각각 103/897, 77/923, 54/946이었다. INVALID_HANDLE, NOT_FOUND, IllegalStateException 및 다른 submit 결과는 모두 0개였다. BUSY를 받은 회차는 submit을 다시 제출하지 않고 submit이 모두 돌아온 뒤 resource cleanup에서 close했다.

Framework는 bclose/framework-scratch-run2에만 복사해 실행했다. /root/worktree/zlink-jdr의 Java source를 복사했고 saved jdr6/change.diff의 기반 blob으로 대상 파일을 복원한 뒤 patch를 적용했다. 현재 zlink-jdr은 첫 보고서 때의 미커밋 변경이 이미 commit된 상태여서 현재 diff가 비어 있었다. git diff <기반 blob> HEAD:<파일>의 역 patch를 사용했으며 다른 git 작업은 하지 않았다. production 두 파일은 jdr6/change.diff에 기록된 결과 blob hash와 일치함을 확인했다. 기록은 framework-patch-run2.log에 있다.

현재 jdr6 저장 test와 첫 보고서의 저장 test는 경합 외 helper의 closeDeadline 생성자 사용 등이 달랐다. 첫 보고서와 동일한 재현을 위해 framework-fixture-used.java.txt를 scratch에 그대로 사용했고 framework-fixture-run2.java.txt에도 보존했다. 차이는 framework-base-comparison-run2.diff에 있다. 두 경합 메서드, requester 분리, 허용 결과와 assertion은 동일하다. 첫 보고서와 같이 반복 횟수 1,000 및 결과 집계와 loaded binding 위치 출력만 포함한다. 실제 Framework worktree와 source는 수정하지 않았다.

scratch 최초 build는 복사하지 않은 framework/runtime/protocol/generated/jvm source 때문에 컴파일 실패했다. framework-races-run2-setup-first.log에 보존했다. Gradle sourceSets가 참조하는 protocol source와 resource를 scratch에 복사한 뒤 같은 명령을 다시 실행해 통과했다. 이 실패는 테스트 실행 전의 scratch 구성 오류였다.

Framework 명령은 scratch의 framework/languages/java에서 ./gradlew --no-daemon --max-workers=2 -Pzlink.includeSamples=false :zlink-framework-core:test --tests '*ZLinkJavaRawServicePortContractTest.repliesRacing*'였다. ZLINK_JAVA_BINDINGS_SOURCE=/root/worktree/zlink-4060-binding-close/bindings/java로 기존 dependency substitution을 사용했다. 로드된 jar는 /root/worktree/zlink-4060-binding-close/bindings/java/build/libs/zlink-java-1.23.0.jar였다. framework-races-run2-1000.log, framework-outcomes-run2.txt, results/framework-races-run2-1000/에 결과를 보존했다.

Framework port close는 1,000/1,000회 통과했다. reply 8,000개는 OK 4,196개, Framework의 IllegalStateException("service port is closed") 3,802개, binding TERMINATED/108 2개였다. runtime close도 1,000/1,000회 통과했다. reply 8,000개는 OK 7,571개와 같은 Framework 종료 거절 429개였다. 양쪽 모두 close 성공 assertion이 통과했다. INVALID_HANDLE, NOT_FOUND, binding IllegalStateException("socket is closed")은 모두 0개였다. JUnit 합계는 2,000개 통과, skip과 실패 0개였고 테스트 시간은 55.829초였다. Gradle 실행 시간은 3분 38초였다.

RequestRetryCloseContractTest.java는 main의 import 순서와 변경하지 않은 줄을 복원하고 BUSY 확인, native retry 성공, 반환 후 close라는 의미 변경만 남겼다. SubmitCloseAdmissionContractTest.java와 SubmitCloseRaceContractTest.java는 같은 import 배치, 80열과 4칸 continuation으로 작성했다. 세 파일에 80열을 넘는 줄은 없다. google-java-format은 실행하지 않았다.

git diff --check가 통과했다. diff-check-run2.log에 exit 0을 기록했다. 신규 두 파일의 git diff --no-index --check도 공백 오류가 없으며 new-files-whitespace-run2.log에 기록했다. change.diff는 tracked 네 파일과 신규 test 두 파일을 포함한 전체 diff다.

변경 파일은 CompletionOwner.java, NativeSocketRuntime.java, CompletionNativeFixture.java, RequestRetryCloseContractTest.java, SubmitCloseAdmissionContractTest.java, SubmitCloseRaceContractTest.java다. 첫 수정의 NativeSocketRuntime과 fixture 의미 변경은 유지했다. 임시 main 복원 디렉터리와 Framework scratch는 결과를 보존한 뒤 삭제했다. 남은 테스트 실패는 없다.

4. PUB publish 대 close에도 같은 해제 핸들 사용 구간이 있다.

bindings/java/src/main/java/systems/zlink/runtime/sockets/SocketSendPlane.java:227, :230, :236, :238, :265, :267, :294.
bindings/java/src/main/java/systems/zlink/runtime/sockets/SocketCore.java:58, :62, :243, :245, :261.
bindings/java/src/main/java/systems/zlink/runtime/sockets/NativeSocketRuntime.java:602, :613, :619.
