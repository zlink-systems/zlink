1. 변경 전 publish 호출과 핸들 해제 구간.

이 보고서의 변경 전 줄 번호는 before-*.java.txt에 보존한 branch HEAD 소스 기준이다. 공통 접두 경로는 bindings/java/src/main/java/systems/zlink/runtime/sockets/다.

PUB와 XPUB는 SocketCore.java:58-62의 CompletionOwner 생성 조건에 포함되지 않는다. public PubSocket.publish는 NativePubSocket.java:49-62의 TopicSendInvoker.submit에서 NativeSocketRuntime.java:408-414로 들어간다. flags NONE은 TopicPlane.java:55-59와 NativeSocketRuntime.java:802-804를 거쳐 SocketSendPlane.java:225-231의 publishParts로 간다. flags DONT_WAIT는 NativeSocketRuntime.java:410-411, :425-426과 TopicPlane.java:68-71을 거쳐 SocketSendPlane.java:234-239의 publishNoWaitPartsResult로 간다. public builder는 part 하나도 List로 제출한다.

두 multipart 진입점의 SocketSendPlane.java:227, :236은 socket.ensureOpen을 부르며 SocketCore.java:261-263이 NativeSocketRuntime.java:602-603의 plain handle 값을 검사한다. :230, :238에서 submitPartsAttempt로 들어가기 전에 native topic을 준비한다. SocketSendPlane.java:283-294가 thread별 parts scratch를 준비하고 메시지를 이동한 다음 submitter를 호출한다. submitter는 :265-268에서 socket.handle을 읽어 Native.publishNoWaitCritical 또는 Native.publish에 넘긴다. Native.java:990-996의 critical downcall과 :976-984의 일반 downcall은 모두 zlink_publish를 호출한다. Native.java:239-250이 그 ABI 바인딩이다.

단일 frame 경로도 있다. TopicPlane.java:44-48 및 :62-65, NativeSocketRuntime.java:397-398 및 :421-422에서 SocketSendPlane.java:149-163으로 들어간다. :155와 :162의 submitSingleAttempt는 :273-280에서 message handle을 가져와 같은 :265-268의 submitter를 호출하고 메시지 이전을 기록한다. 이 경로에는 plain ensureOpen 검사도 없다. handle을 쓰는 native 호출은 두 publish downcall뿐이며 messageMoveTo, messageNativeHandle, messageMarkTransferred, native topic 인코딩은 socket handle을 쓰지 않는다.

close는 NativeSocketRuntime.java:610-624에서 SocketCore.java:243-246으로 들어간다. CompletionOwner가 없으면 :245가 Native.close(socket.handle)를 직접 실행하며, native close가 반환된 뒤 NativeSocketRuntime.java:619가 handle을 NULL로 바꾼다. publish의 open 검사와 native entry 사이에 close가 완료될 수 있다. publish가 handle 값을 읽은 뒤 native entry 전에 close가 완료될 수도 있다. NULL 또는 해제된 pointer가 Core에 전달되는 구간이다.

다른 경합 가능 경로는 아래 위치에서 확인했으며 수정 범위에서 제외했다. 이 경로를 동적으로 재현했다고 판정하지 않는다.

SocketCore.java:68, :78, :88, :98, :119의 bind/connect/unbind/disconnect/disconnectRid와 :232의 monitorOpen은 handle을 직접 사용한다. SocketOptionSupport.java:60, :62, :66 및 :269, :271, :275의 PUB/SUB/common option setter와 getter, :163, :169의 int option 전달, :196, :213의 routing ID 설정과 조회도 예약이 없다. :203의 receive flow state 변경과 :238-239의 subscription 설정과 해제도 같은 형태다. NativeSocketRuntime.java:904의 subscriptionAt는 한 번의 조회 및 BUFFER_TOO_SMALL 재호출마다 raw handle을 사용한다.

TopicPlane.java:117, :129의 XPUB subscription event 수신과 :150, :211의 SUB/XSUB subscribe 수신도 open 검사 후 예약 없이 handle을 사용한다. ReceivePlane.java:216, :219의 일반 receive downcall도 예약이 없으나 PUB 계열의 공개 수신 표면은 위 TopicPlane 경로를 사용한다. NettySocketPlane은 이 ReceivePlane으로 전달하며 별도 native handle 호출은 없다.

runtime/core/NativeCoreRuntime.java:42-45의 proxy 입력·출력·capture, NativeSocketBase.java:221의 connectionAttemptsSnapshot과 NativePoller.java:112-115, :158, :354-357의 socket 등록·변경·제거도 추출한 socket handle을 native로 넘기는 지점이다. poller 자체 handle의 수명과는 별개로 socket handle 추출과 native 호출 사이의 예약은 없다. 생성 실패 정리인 NativeSocketRuntime.java:89, :112는 공개 전 constructor 실패 정리이므로 publish-close 경합 대상이 아니다.

2. 공통 예약 소유자와 순서.

SocketCore로 기존 ownerLock, closed, activeNativeCalls를 이동한다. CompletionOwner는 같은 SocketCore의 lock과 상태를 사용한다. PUB 전용 count나 closed flag, lock, queue는 추가하지 않는다. CompletionOwner의 core 필드는 기존 소유 상태에 접근할 참조이며 두 번째 상태가 아니다. NativeSocketRuntime.socketCore는 기존 객체를 반환한다.

withNativeCall은 ownerLock을 한 번 잡아 하나의 ensureOpenForSubmitLocked predicate로 TERMINATED/ESHUTDOWN을 결정하고 count를 증가시킨다. native topic 준비, scratch 준비, 메시지 이동, native 호출과 errno 확보는 잠금을 놓은 뒤 수행한다. finally에서 atomic count를 감소시킨다. CompletionOwner의 기존 token/pending 등록은 이미 잡은 같은 lock 안에서 검사와 할당을 마치고 count를 증가시킨다. 그 경로에 withNativeCall을 다시 넣지 않는다.

closeNativeHandle은 같은 lock 안에서 count를 읽는다. 예약이 남아 있으면 BUSY/EBUSY로 반환하고 handle을 해제하지 않는다. count가 0이면 lock을 유지한 채 Native.close를 호출하며 OK일 때만 closed를 기록한다. Core 자체 BUSY나 다른 실패에서는 closed를 바꾸지 않는다.

publish의 예약 증가가 먼저이면 close는 예약 해제 전까지 count가 0인 상태를 볼 수 없다. 여러 publish의 증감은 AtomicInteger로 합산되므로 일부 호출의 해제로 다른 호출의 예약이 사라지지 않는다. close가 count 0을 먼저 확인하면 같은 lock 때문에 새 예약이 들어올 수 없다. native close와 closed 기록이 끝난 뒤 들어온 submit은 closed를 보고 native 준비 전에 TERMINATED로 끝난다. 성공한 예약이 살아 있는 동안 handle=NULL 쓰기에 도달할 수 없으므로 handle을 volatile로 중복 관리하지 않아도 이 경로의 handle 읽기는 해제와 겹치지 않는다. atomic 감소는 native 호출 반환 및 errno 확보 뒤에 실행한다.

대안으로 closed bit와 count를 한 atomic word에 넣는 방법을 검토했다. 이 방법은 CompletionOwner의 기존 token/pending lock과 CAS 사이 순서를 새로 설명해야 한다. 기존 잠금을 공통 소유자로 옮기는 방법은 그 등록과 예약을 같은 임계 구역에 유지하므로 채택했다. native 실행 전체를 잠그는 방법은 concurrent publish를 직렬화하므로 채택하지 않았다.

일반 publish, DONT_WAIT publish, 단일 frame의 일반·DONT_WAIT 각각 main의 lock 진입은 0회이며 수정 뒤는 1회다. release에는 lock 진입이 없다. 기존 CompletionOwner token/pending 예약은 lock 진입 횟수가 늘지 않는다. native 호출 동안 lock을 잡지 않으므로 한 publish를 준비 중에 멈춰도 다른 publish가 Core로 들어갈 수 있어야 한다.

소유 계층은 Java binding SocketCore의 handle 예약이며 Core는 실제 API admission을 계속 소유한다.
Spec 근거는 bindings/doc/spec/README.ko.md:498의 독립 record 동시 제출, :1851과 기존 보고서의 :2845 종료 submit 계약, core/doc/spec/core/socket/README.ko.md:58 및 :647-657의 EBUSY·ESHUTDOWN·해제 pointer 계약이다.
교차언어 대조는 bclose/report.md에 기록된 .NET/C++의 같은 handle 수명 공백과 Node의 일반 동기 경로 조사 결과를 참고했다. 이번 변경은 Java binding에 한정하며 Framework 동작이나 다른 binding을 수정하지 않는다.
변경 분류는 B이며 기존 handle 수명 계약 위반 수정이다.

예약 위치는 SocketCore.java:267-277의 withNativeCall이며 :270에서 공통 종료 predicate를 부르고 :271에서 증가시킨다. 실제 증가는 :287-288, 원자적 해제는 :291-292다. close의 예약 충돌 검사는 :257이며 :260의 Native.close 성공 뒤 :262에서만 closed를 기록한다. predicate는 :280-284 한 곳이다. SocketSendPlane.java:154, :163, :231, :239의 네 publish 진입이 같은 withNativeCall을 사용하며 native topic 준비도 그 안에 있다.

CompletionOwner.java:394-401의 register, :406-410의 nextContextToken, :415-423의 nextSendContextToken은 기존 token/pending lock을 재사용한다. 각각 :395, :407, :416에서 같은 predicate로 검사하고 fallible 준비 뒤 :400, :409, :422에서 예약한다. 비동기 send :113, 비동기 request :182, 동기 request :243, no-wait send :289의 finally에서 해제한다. 동기 send :161, reply :261, writable send 재제출 :742, writable request 재제출 :783은 공통 withNativeCall과 :276의 finally를 사용한다. 기존 SocketSendPlane의 blocking send 일곱 곳도 같은 공통 wrapper를 사용한다. 기존 token/pending 예약 뒤 다시 예약하는 경로는 추가하지 않았다.

수정 전/후 규칙 수는 published socket의 native close 경로 2곳에서 1곳, TERMINATED/ESHUTDOWN 판정 1곳에서 1곳, 예약 충돌 BUSY 판정 1곳에서 1곳이다. close 성공 시 closed 기록은 CompletionOwner의 두 위치에서 SocketCore의 한 위치로 줄었다. CompletionOwner가 있는 socket은 기존 필드 세 개의 위치만 바뀐다. PUB/XPUB/SUB/XSUB도 같은 공통 필드를 갖게 되며 CompletionOwner 쪽 필드는 삭제했다. PUB 전용 상태나 두 번째 lock/count는 추가하지 않았다. 기존 CompletionOwner의 finalized는 pending 정리의 중복 실행을 막는 별도 기존 상태다.

3. 검증 결과.

변경 전 branch HEAD는 18a65e2dfd다. 모든 build와 test는 /root/jobs/fx/cx/measure-shared.sh python3 bpub/run-loaded.py로 실행했다. runner 안에서 stress-ng --cpu 16 --cpu-method loop --metrics-brief를 실행하고 각 명령이 끝날 때 stress-ng에 SIGTERM을 보냈다. 모든 load process의 종료 코드는 0이다. 각 명령의 시작부터 종료까지 2초 간격으로 MemAvailable을 기록했다. loop 제한은 build를 포함해 39분이며 실제 각 명령은 그보다 짧았다. 공용 측정 잠금 대기 시간은 runner 실행 시간에 포함하지 않았다.

Java native 환경은 ZLINK_LIBRARY_PATH=/root/.cache/zlink/core/1.23.0/linux-x64/lib/libzlink.so와 ZLINK_CORE_PACKAGE_PREFIX=/root/.cache/zlink/core/1.23.0/linux-x64다. Core SHA-256은 6f496304ef6e1287f57234567231bbd2b76ad2392f99e816001417d01215bf01이다. 변경 전 compiled class hash는 before-sha256.json에 보존했다.

변경 전 명령은 ./gradlew --no-daemon --max-workers=2 :test --tests '*PublishCloseRaceContractTest'다. publish-before-1000.log, publish-before-1000-results.json, results/publish-before-1000/에 결과를 보존했다. production 소스에 diff가 없는 상태에서 실행했다. 네 경로를 각각 1,000회 실행하며 회차마다 같은 PUB handle에서 publish thread 네 개와 close thread 한 개를 동시에 시작했다. 매 회 새 PUB handle을 만들고 publish는 다시 제출하지 않았다. public multipart 두 경로는 part 두 개를 사용하며 단일 frame 두 경로는 기존 runtime 진입을 직접 사용한다. assertion은 성공 또는 TERMINATED/ESHUTDOWN만 허용하며 다른 RuntimeException도 합산한 뒤 실패한다.

변경 전 PUBLISH 4,000건은 OK 2,925건, TERMINATED/108 120건, INVALID_HANDLE/14 107건, IllegalStateException 848건이었다. PUBLISH_NO_WAIT는 OK 3,396건, TERMINATED/108 31건, INVALID_HANDLE/14 19건, IllegalStateException 554건이었다. FRAME은 OK 3,285건, TERMINATED/108 71건, INVALID_HANDLE/14 644건이었다. FRAME_NO_WAIT는 OK 3,310건, TERMINATED/108 80건, INVALID_HANDLE/14 610건이었다. 네 close 경합은 각각 1,000회 모두 OK였다. 총 INVALID_HANDLE/EFAULT 1,380건과 IllegalStateException 1,402건으로 네 테스트가 모두 실패했다. NOT_FOUND와 그 밖의 결과는 0건이었다. 실행 시간은 48.14초였다.

변경 전 admission 명령은 ./gradlew --no-daemon --max-workers=2 :test --tests '*PublishCloseAdmissionContractTest'다. admission-before.log, admission-before-results.json, results/admission-before/에 기록했다. 일반 publish와 no-wait publish를 메시지 이동 직전에 멈춘 동안 close가 성공하여 BUSY assertion 두 개가 실패했다. 종료 후 publish도 ZlinkSubmitException 대신 IllegalStateException을 던져 실패했다. 준비 예외 뒤 close 검사는 통과했다. 합계 4개 중 3개 실패이며 실행 시간은 39.03초였다.

수정 후 최소 검증 명령은 ./gradlew --no-daemon --max-workers=2 :test --tests '*PublishCloseAdmissionContractTest' --tests '*SubmitCloseAdmissionContractTest' --tests '*ConcurrentAdmissionContractTest' --tests '*RequestRetryCloseContractTest'다. admission-after.log, admission-after-results.json, results/admission-after/에 결과가 있다. 새 PUB 검사 4개와 기존 검사 7개가 모두 통과했다. 멈춘 publish와 별개인 publish가 먼저 반환하여 동시 실행이 직렬화되지 않음을 확인했다. 준비 중 close는 BUSY/EBUSY이며 준비를 풀어 native publish가 반환한 뒤 close는 성공했다. PUB와 XPUB의 종료 후 submit, 단일 frame 두 진입은 모두 TERMINATED/ESHUTDOWN을 내고 입력 메시지를 보존했다. 일반·no-wait 준비 예외 뒤 close도 성공했다. 기존 Core BUSY 후 재사용, token/pending 준비 예외 해제, reply/request/send 종료 거절, request retry 및 다른 제출의 동시 진행도 통과했다. 실행 시간은 53.03초였다.

수정 후 경합 명령은 ./gradlew --no-daemon --max-workers=2 :test --tests '*PublishCloseRaceContractTest' --tests '*SubmitCloseRaceContractTest'다. races-after-1000.log, races-after-1000-results.json, results/races-after-1000/에 결과를 보존했다. 변경 전과 같은 PUB test source와 assertion을 사용했다. 일곱 경합을 각각 1,000회 실행하여 7개 테스트가 모두 통과했다. 실행 시간은 53.06초였다.

수정 후 PUBLISH 4,000건은 OK 3,417건과 TERMINATED/108 583건이며 close의 BUSY/OK는 120/880회였다. PUBLISH_NO_WAIT는 OK 2,899건과 TERMINATED/108 1,101건이며 close는 111/889회였다. FRAME은 OK 3,388건과 TERMINATED/108 612건이며 close는 109/891회였다. FRAME_NO_WAIT는 OK 3,170건과 TERMINATED/108 830건이며 close는 66/934회였다. PUB 16,000건에서 INVALID_HANDLE, NOT_FOUND, IllegalStateException 및 다른 결과는 0건이었다.

기존 SEND 4,000건은 OK 3,420건과 TERMINATED/108 580건이며 close는 BUSY/OK 40/960회였다. REQUEST는 OK 3,217건과 TERMINATED/108 783건이며 close는 36/964회였다. REPLY는 OK 3,365건과 TERMINATED/108 635건이며 close는 54/946회였다. 다른 결과는 0건이었다. BUSY 회차는 제출을 반복하지 않고 모든 submit 반환 뒤 resource cleanup에서 close했다.

전체 binding 명령은 ./gradlew --no-daemon --max-workers=2 --continue test integrationTest다. binding-full.log, binding-full-results.json, results/binding-full/에 결과가 있다. root test 181개, integration 36개, Netty 3개, Kotlin contract 4개, perf-multi 23개로 합계 247개다. 245개 통과, 기존 skip 2개이며 failure와 error는 0개다. 기존 skip은 MonitorConnectionIdentityContractTest.inprocClosedKeepsReadyConnectionIdentity와 LibraryLoaderTest.coreWithoutSnapshotExportIsRejectedBeforeContextCreation이다. 전체 runner 실행 시간은 129.07초이며 Gradle은 2분 7초에 완료됐다.

전체 suite에서도 각 경합을 1,000회씩 실행했다. PUBLISH는 OK/TERMINATED 2,868/1,132건, PUBLISH_NO_WAIT는 3,346/654건, FRAME은 3,299/701건, FRAME_NO_WAIT는 3,166/834건이었다. 각 경로 4,000건이며 close BUSY/OK는 순서대로 112/888, 101/899, 151/849, 90/910회였다. SEND는 OK/TERMINATED 3,088/912건, REQUEST는 3,523/477건, REPLY는 2,881/1,119건이며 close BUSY/OK는 각각 101/899, 93/907, 26/974회였다. 모든 TERMINATED의 errno는 108이었다. INVALID_HANDLE, NOT_FOUND, IllegalStateException 및 다른 submit 결과는 0건이었다.

MemAvailable 최저값은 publish-before-1000에서 23,930,672 KiB인 22.82207 GiB, admission-before에서 20,620,240 KiB인 19.66499 GiB, admission-after에서 15,595,852 KiB인 14.87336 GiB, races-after-1000에서 18,475,656 KiB인 17.61976 GiB, binding-full에서 15,634,192 KiB인 14.90993 GiB였다. 모든 실행에서 12 GiB 이상이었다. 각 실행명에 -memory.log, -load.log, -summary.json을 붙인 파일이 표본, 부하 종료 기록과 명령·종료 코드 기록이다. 다섯 부하 실행 모두 SIGTERM 후 16개 CPU stressor가 성공으로 정리됐다.

after-sha256.json에 최종 production source, compiled class, jar와 Core hash를 기록했다. Core hash는 변경 전과 같고 최종 jar의 SocketCore, CompletionOwner, SocketSendPlane, NativeSocketRuntime class가 worktree compiled class와 byte 단위로 같음을 확인했다. 새 테스트 두 파일과 수정한 기존 admission 테스트는 80열과 4칸 continuation, 이웃 테스트의 import 순서를 따랐다. google-java-format은 실행하지 않았다. git diff --check는 exit 0이며 신규 두 파일의 no-index whitespace 검사도 진단이 없다. no-index의 exit 1은 신규 파일 diff가 존재한다는 뜻이다. 기록은 diff-check.log와 style-check.log에 있다. 남은 테스트 실패는 없다.

4. 중복 검사와 전달 점검.

CompletionOwner.java의 변경 전 :97-101에 있던 closed와 count, :86의 ownerLock을 SocketCore.java:55-59로 이동했다. 변경 전 :386과 :872의 closed 중복 기록에서 최종 :262 한 곳만 남겼다. 변경 전 SocketCore.java:243-246의 타입별 close 분기와 CompletionOwner.java:377-388의 실제 close 처리를 SocketCore.java:252-265 한 곳으로 합쳤다.

변경 전 CompletionOwner.java:275-286의 예약 wrapper는 최종 SocketCore.java:267-277로 이동했다. SocketSendPlane과 CompletionOwner의 native 호출자가 그 소유자로 바로 들어가므로 CompletionOwner에 전달 전용 wrapper를 남기지 않았다. 변경 전 CompletionOwner.java:938의 NativeSendAction 인터페이스는 제거하고 표준 Supplier를 사용한다. 변경 전 SocketSendPlane.java:227, :236의 plain ensureOpen 전달 두 곳은 공통 predicate로 대체했다. 새 closed check나 예약 count를 PUB 경로에 복제하지 않았다.

유지한 검사는 다음과 같다. 변경 전 CompletionOwner.java:729의 public owner 검사 전 closed 확인과 :435의 pending 등록 전 closed 확인은 최종 :688-691 및 :394-401에서 같은 predicate를 사용한다. 두 임계 구역 사이에 close가 가능하므로 이 두 경계의 확인은 유지했다. CompletionOwner.java:529, :552, :561, :618, :702, :1133의 기존 completion drain·retry·public owner 경계는 같은 core.isClosed를 조회하며 handle 예약을 재구현하지 않는다. completionRecv 자체의 별도 close 경합은 이 변경에서 해결했다고 판정하지 않는다.

SocketSendPlane.java:208, :221의 raw send ensureOpen은 기존 send 진입 검사이며 PUB publish 대상에서 제외했다. CompletionOwner.java:357의 validateParts와 SocketSendPlane.java:376의 validateParts는 각각 제출과 기존 raw 송신 준비에 사용하며 null part의 기존 예외 타입이 다르다. 이 변경에서 검증 계약을 합치거나 메시지 이동 정책을 바꾸지 않았다. NativePubSocket의 publish invoker에서 TopicPlane과 SocketSendPlane으로 전달하는 기존 경로도 public 형태와 native 준비 경계를 유지했다.

점검 결과는 socket 종류별 close 처리 2곳을 1곳으로 통합, closed 기록 2곳을 1곳으로 축소, PUB의 plain open 확인 전달 2곳 제거, 전달 wrapper와 custom 함수 인터페이스 제거다. 남겨 둔 중복 확인과 다른 handle 경합은 위 file:line에 기록했으며 새 Issue나 spec 변경은 없다.

변경 파일은 bindings/java/src/main/java/systems/zlink/runtime/sockets/의 SocketCore.java, CompletionOwner.java, SocketSendPlane.java, NativeSocketRuntime.java와 bindings/java/src/test/java/systems/zlink/runtime/sockets/의 SubmitCloseAdmissionContractTest.java, PublishCloseAdmissionContractTest.java, PublishCloseRaceContractTest.java다. change.diff는 branch HEAD 상대 diff이며 신규 두 파일을 포함한다. Core, 다른 binding과 spec은 수정하지 않았다. commit, branch 조작과 GitHub 작업은 수행하지 않았다.

PR 본문에 반영할 내용은 다음과 같다. Java PUB/XPUB publish에는 CompletionOwner 부재로 released handle을 native에 전달하는 구간이 있었다. 이 변경은 SocketCore의 공통 예약으로 일반 publish와 DONT_WAIT publish, 단일 frame 경로를 보호하며 이 PUB 문제에 남은 blocker는 없다. publish 외 handle 사용의 close 경합은 이 보고서 1절의 위치에서 보고만 했으며 해결 범위에 포함하지 않았다.
