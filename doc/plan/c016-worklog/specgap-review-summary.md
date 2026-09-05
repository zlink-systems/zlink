codex
**최종 의견: 1번 수정 채택, 2번 수정 채택, 3번 채택입니다.** 특히 2번의 “이전 pipe의 request는 자기 timeout으로 끝난다”는 기존 계약의 예외를 누락하므로 그대로 채택하면 안 됩니다.

`main` 작업 파일을 읽기 전용으로 검토했습니다. 종료 시 HEAD는 `09967a7d76`이며, 파일 변경·빌드·테스트는 실행하지 않았습니다. 아래 테스트 언급은 기존 소스·보고서를 검토한 결과입니다. 세션의 파일시스템 쓰기 금지로 진행 로그와 요약 파일은 생성·append하지 못했습니다.

**1. inproc peer close의 `CLOSED`: 발생 범위를 좁혀 수정 채택**

**(a) 계약 근거**

- [06-monitoring.ko.md:69](/home/hep7hep7/project/zlink/core/doc/spec/core/06-monitoring.ko.md:69): `connection_id`는 “하나의 물리적 transport 시도를 식별하는 진단·correlation 값”이며 reconnect fence가 아니다.
- [06-monitoring.ko.md:105](/home/hep7hep7/project/zlink/core/doc/spec/core/06-monitoring.ko.md:105): 같은 monitor의 commit 순서는 보장하지만, 서로 다른 connection의 wall-clock 순서는 보장하지 않는다. Queue는 bounded·lossy다.
- [socket/README.ko.md:609](/home/hep7hep7/project/zlink/core/doc/spec/core/socket/README.ko.md:609): socket close는 자원과 pending operation·unread completion을 정리한다. 이 계약은 모든 연결에서 `CLOSED` event를 받아야 한다고 규정하지 않는다.
- `CLOSED` enum은 있으나 transport별 발생 조건은 정의되어 있지 않다.

**(b) 권고안과 구현·Framework의 관계**

**inproc에 `CLOSED`를 추가하지 않는 방향은 타당합니다. 다만 “물리 transport(fd/listener) close”는 너무 넓습니다.**

현재 `event_closed` 호출은 TCP/IPC/WS/TLS의 **connecter와 listener**에 있습니다. 이미 연결된 공통 ASIO engine의 오류·단절 경로는 [`asio_engine.cpp:1938`](/home/hep7hep7/project/zlink/core/src/runtime/engine/asio/asio_engine.cpp:1938)에서 `DISCONNECTED`를 냅니다. 따라서 “모든 fd 해제마다 `CLOSED`”로 정의하면 기존 network transport에도 추가 구현이 필요합니다.

기존 identity 테스트도 연결 종료는 `READY → DISCONNECTED`, 실패한 접속 시도는 `CONNECT_DELAYED → CLOSED`로 대조합니다. [test_monitor_connection_identity.cpp:118](/home/hep7hep7/project/zlink/core/tests/integration/monitoring/test_monitor_connection_identity.cpp:118), [동일 파일:147](/home/hep7hep7/project/zlink/core/tests/integration/monitoring/test_monitor_connection_identity.cpp:147).

Framework는 inproc의 `CLOSED`를 필수로 기다리지는 않습니다.

- .NET ClientServer는 `Disconnected`와 `Closed`를 모두 처리합니다. [ZLinkClientServerClientRuntime.cs:1018](/home/hep7hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkClientServerClientRuntime.cs:1018).
- .NET Mesh는 `Disconnected`만 연결 후보 제거에 사용합니다. [ZLinkManagedMeshNode.cs:8643](/home/hep7hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:8643).
- Java Mesh는 두 event를 terminal 처리하고 endpoint별 후보 queue에서 항목을 제거합니다. `CLOSED`를 추가하면 같은 연결을 두 번 정리하지 않는지 검토해야 합니다. [ZLinkJavaRawMeshNode.java:6710](/home/hep7hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:6710), [동일 파일:7157](/home/hep7hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:7157).

별도로 WS/TLS connecter는 event마다 endpoint pair를 생성하는 코드가 남아 있습니다. 이는 같은 attempt의 ID 유지 계약과 불일치할 가능성이 있는 **기존 구현 문제**이며, inproc `CLOSED`의 정책 선택과 분리해야 합니다. [WS:438](/home/hep7hep7/project/zlink/core/src/runtime/transports/ws/asio_ws_connecter.cpp:438), [TLS:411](/home/hep7hep7/project/zlink/core/src/runtime/transports/tls/asio_tls_connecter.cpp:411). 이번 검토에서는 실행 확인하지 않았습니다.

**(c) 대안 비교**

| 선택 | 장점 | 단점·영향 범위 |
|---|---|---|
| connecter/listener 자원 해제 event로 한정 | 기존 발생 범위와 맞고 ABI·runtime 변경 불필요 | transport 공통 단절 신호는 `DISCONNECTED`라고 설명해야 함 |
| 모든 transport lifecycle 종료에 `CLOSED` | event 이름의 직관성이 높음 | inproc뿐 아니라 network engine도 검토·수정 필요. `DISCONNECTED`와 순서·중복·미연결 attempt 의미를 새로 정의해야 함. 모든 binding monitor 계약·Framework terminal 처리 영향 |

**(d) 최종 의견**

**수정 채택.** `CLOSED`는 connecter/listener가 소유한 OS transport 자원의 해제 통지로 한정하십시오. “inproc은 DISCONNECTED로 끝난다”도 **해당 연결의 단절 관찰**이라는 범위가 필요합니다. Connect intent가 살아 있으면 이후 자동 재연결은 계속됩니다.

**(e) Spec 문장 초안**

> `CLOSED`는 TCP·IPC·WS·TLS의 접속 시도 또는 listener가 소유한 OS transport handle을 닫았음을 알리는 event다. 이미 성립한 연결의 단절은 `DISCONNECTED`로 보고하며, 그 연결에 추가 `CLOSED`가 반드시 발생하는 것은 아니다. Inproc peer의 단절은 `DISCONNECTED`로 보고하고 `CLOSED`는 발생시키지 않는다. `DISCONNECTED`는 connect intent의 제거를 뜻하지 않으며 자동 재연결 여부는 connect/disconnect 계약을 따른다. Monitor event의 소비는 연결 종료나 재연결 진행의 전제 조건이 아니다.

---

**2. 즉시 disconnect→connect: overlap은 허용하되 request 결과를 일괄 timeout으로 고정하지 말 것**

**(a) 계약 근거**

현재 계약에는 서로 다른 종료 상황이 이미 구분되어 있습니다.

| 상황 | 기존 계약 |
|---|---|
| 같은 RID, 같은 방향의 중복 연결 | REJECT는 기존 pipe 유지, HANDOVER는 새 pipe가 인수 |
| 반대 방향 충돌에서 물러나는 pair | admit된 request를 즉시 `NOT_CONNECTED`로 종료. 자기 timeout까지 기다리지 않음 |
| admission 이후 일시적 물리 단절 | payload replay 없이 기존 correlation·timeout budget 유지 |
| endpoint 또는 logical RID 명시적 제거 | REQUEST는 `NOT_FOUND` |
| responder의 reply 경로 | 같은 logical RID의 현재 ready pipe 사용. 물리 단절만으로 reply token을 무효화하지 않음 |

근거: [socket/README.ko.md:159](/home/hep7hep7/project/zlink/core/doc/spec/core/socket/README.ko.md:159), [동일 파일:1059](/home/hep7hep7/project/zlink/core/doc/spec/core/socket/README.ko.md:1059), [동일 파일:1129](/home/hep7hep7/project/zlink/core/doc/spec/core/socket/README.ko.md:1129), [07-router.ko.md:291](/home/hep7hep7/project/zlink/core/doc/spec/core/socket/07-router.ko.md:291), [동일 파일:317](/home/hep7hep7/project/zlink/core/doc/spec/core/socket/07-router.ko.md:317).

[03-errors.ko.md:359](/home/hep7hep7/project/zlink/core/doc/spec/core/03-errors.ko.md:359)도 `TIMED_OUT`, `NOT_FOUND`, `NOT_CONNECTED`를 별개 결과로 정의합니다.

**(b) 권고안의 문제와 Framework 대조**

첫째, **monitor 소비와 물리 종료를 구분해야 합니다.** Event가 queue에 있지만 아직 읽히지 않은 상태와, 이전 연결이 실제로 종료 중인 상태는 다릅니다. Lossy monitor 소비를 Core connect의 진행 조건으로 삼으면 안 됩니다. Polling 계약의 내부 command 처리·lost-wake 보장도 그대로 유지해야 합니다. [05-polling.ko.md:73](/home/hep7hep7/project/zlink/core/doc/spec/core/05-polling.ko.md:73).

둘째, **기존 테스트는 “request가 이전 pipe로 갔다”를 직접 증명하지 않습니다.** Request는 disconnect→connect **이후** 제출됩니다. 따라서 disconnect 전에 admit된 pending request와 다른 사례입니다. 테스트 끝의 `reply_connection`은 opaque token 때문에 추론한 값이며, 성공한 reply만으로 request 송신 경로까지 같은 connection이었다고 입증할 수 없습니다. [test_socket_disconnect_progress_without_app_poll.cpp:447](/home/hep7hep7/project/zlink/core/tests/integration/test_socket_disconnect_progress_without_app_poll.cpp:447), [동일 파일:515](/home/hep7hep7/project/zlink/core/tests/integration/test_socket_disconnect_progress_without_app_poll.cpp:515).

REJECT에서 새 연결이 **상대편의 아직 남은 동일 RID 등록**과 충돌하는 상황도 구분해야 합니다. 이는 local requester가 이미 제거한 pipe를 다시 선택했다는 주장과 다릅니다.

셋째, **Core overlap 허용만으로 Framework 대기를 삭제할 수 없습니다.** 현재 Framework wire spec은 다음을 명시합니다.

> “그 endpoint의 close snapshot 또는 disconnect event를 받기 전에는 같은 endpoint에 새 connection을 만들지 않는다.”

[06-wire-protocol.ko.md:335](/home/hep7hep7/project/zlink/framework/doc/framework/common/spec/server/02-channel-transport/06-wire-protocol.ko.md:335).

실제 구현에도 차이가 있습니다.

- .NET ClientServer는 terminal 관찰을 기다리고, 그동안 별도 poller를 돌립니다. [ZLinkClientServerClientRuntime.cs:1640](/home/hep7hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkClientServerClientRuntime.cs:1640).
- Java ClientServer는 같은 lock 안에서 즉시 disconnect→connect합니다. [ZLinkChannelSocketRegistry.java:1091](/home/hep7hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelSocketRegistry.java:1091).
- .NET Channel/Mesh와 Java Channel의 ROUTER 구성에는 HANDOVER가 켜져 있습니다. [ZLinkChannelBundleFactory.cs:55](/home/hep7hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkChannelBundleFactory.cs:55), [ZLinkManagedMeshNode.cs:289](/home/hep7hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:289), [ZLinkJavaSocketOptions.java:23](/home/hep7hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaSocketOptions.java:23).

Java/.NET 차이는 언어 구조상 불가피한 것으로 확인되지 않았습니다. **현재 Framework 계약과 구현의 불일치로 별도 판정할 사항**입니다. Core의 command progress 수정은 두 번째 poller의 필요성을 없앨 수 있지만, Framework의 교체 관찰 규칙까지 자동으로 없애지는 않습니다.

**(c) 대안 비교**

| 선택 | 장점 | 단점·영향 범위 |
|---|---|---|
| 현재 OK/timeout 관찰을 그대로 계약화 | 당장 구현 비용이 작음 | 원인을 증명하지 않은 “old pipe 귀속”을 고정하고 기존 terminal 규칙을 침해할 위험 |
| 이전 physical terminal까지 Core에서 완전 직렬화 | 호출 순서 설명이 단순함 | local 종료와 remote 등록 제거 중 무엇을 기다리는지 추가 정의 필요. Core endpoint/session·inproc·모든 transport와 binding 호출 지연에 영향 |
| **local 연결 제거와 physical teardown을 분리** | 이전 자원 정리와 새 attempt는 겹칠 수 있고, 새 submit의 local 대상은 명확해짐 | local admission 경계의 구현 검증 필요. 상대편 RID 충돌은 여전히 정책으로 처리해야 함 |

**(d) 최종 의견**

**수정 채택.** 완전한 physical 종료 대기를 강제하기보다는, **disconnect 성공으로 제거한 local 연결을 후속 신규 submit이 다시 선택하지 않는 경계**를 명확히 하는 쪽을 권합니다. 이전 자원의 teardown과 새 attempt는 겹칠 수 있고, 상대편에 남은 동일 RID는 기존 REJECT/HANDOVER 계약으로 처리합니다.

이 local admission 경계는 이번 리뷰의 **설계 권고**이며 현재 구현이 모든 transport에서 만족한다고 검증한 것은 아닙니다. “코드 변경 없음”으로 확정해서는 안 됩니다.

HANDOVER는 동일 logical peer의 교체를 허용하려는 경우 권장할 수 있지만, 모든 request의 성공·무손실을 보장하는 옵션은 아닙니다. 중복 연결을 거부하려는 REJECT의 용도도 유지해야 합니다.

**(e) Spec 문장 초안**

> 성공한 `zlink_disconnect(endpoint)`는 해당 local 연결 등록과 자동 재연결 intent를 제거한다. 후속 신규 submit은 제거된 연결을 admission 대상으로 다시 선택하지 않는다. 관련 physical 자원의 비동기 종료와 이후 `zlink_connect(endpoint)`가 시작하는 새 연결 시도는 겹칠 수 있으며, 새 시도의 진행은 monitor event 소비를 기다리지 않는다. 상대편에 동일 RID의 기존 연결이 남아 있으면 RID duplicate policy를 적용한다. Connect 성공은 remote admission이나 request 전달 성공을 보장하지 않는다.
>
> Request 종료는 completion 계약을 따른다. 명시적 endpoint·logical RID 제거는 `NOT_FOUND`, 반대 방향 handover로 물러나는 pair의 request는 `NOT_CONNECTED`로 처리한다. 일시적 physical disconnect에서는 admission된 payload를 replay하지 않고 기존 correlation과 timeout budget을 유지한다. 유효 reply와 timeout 중 먼저 확정된 결과가 완료를 결정하며, physical disconnect만으로 reply token을 무효화하지 않는다.

Framework wire spec의 관찰 대기 조항을 바꾸려면 별도 계약 정합성 검토가 필요합니다. 해당 보호 문서는 이번 작업에서 변경하지 않았습니다.

---

**3. request/reply physical `connection_id` 공개 API: 추가하지 않는 권고 채택**

**(a) 계약 근거**

- [06-monitoring.ko.md:536](/home/hep7hep7/project/zlink/core/doc/spec/core/06-monitoring.ko.md:536): `connection_id`로 send·reply target을 지정하는 public API는 없다.
- [socket/README.ko.md:287](/home/hep7hep7/project/zlink/core/doc/spec/core/socket/README.ko.md:287): completion record는 operation의 `completion_id`, context, logical RID, 결과·payload를 제공한다.
- [동일 파일:1124](/home/hep7hep7/project/zlink/core/doc/spec/core/socket/README.ko.md:1124): `peer_rid`는 logical peer snapshot이며 reconnect 뒤 physical connection identity로 바뀌지 않는다.
- [07-router.ko.md:294](/home/hep7hep7/project/zlink/core/doc/spec/core/socket/07-router.ko.md:294): reply는 현재 ready Application 또는 Completion pipe를 사용한다.

**(b) 모순 여부**

추가하지 않는 결정은 기존 계약과 맞습니다. 다만 **“진단 필드 추가가 기존 spec상 금지되어 있다”는 뜻은 아닙니다.** 진단용 노출과 라우팅 capability 제공은 다릅니다.

문제는 “request/reply가 사용한 connection ID”가 단일 값으로 명확하지 않다는 점입니다.

- Request가 admission된 연결과 reply가 돌아온 연결은 다를 수 있습니다.
- ROUTER–ROUTER에서는 Application·Completion lane 자체가 다릅니다.
- Timeout에는 reply 수신 연결이 없습니다.
- WRITABLE은 아직 request가 admission되지 않은 대기 토큰입니다.
- ID는 현재 프로세스 범위이므로 remote ID와 직접 비교하는 계약도 아닙니다.

Framework의 physical fence 문제를 이 필드로 해결하려 하면 기존 계층 소유권을 다시 침해합니다. 필요한 동작 판단은 logical identity·admission·liveness와 Core completion 결과로 해야 합니다.

**(c) 대안 비교**

| 선택 | 장점 | 단점·영향 범위 |
|---|---|---|
| **현재 API 유지** | ABI·binding 변경 없음. operation과 physical connection의 책임 구분 유지 | 특정 request의 물리 경로를 public API만으로 직접 입증할 수 없음 |
| completion에 진단 필드 추가 | timeout·handover 분석과 회귀 검증에 도움 | 송신 admission ID·reply 수신 ID·lane·값 부재 의미를 구분해야 함. C layout, Core correlation 보관, C/C++/.NET/Java/Node/Rust/Python/Go binding 변환·계약·테스트 영향 |

Completion은 caller의 `struct_size`를 검증하므로, 필드 추가를 단순한 무비용 확장으로 볼 수 없습니다. [socket/README.ko.md:1116](/home/hep7hep7/project/zlink/core/doc/spec/core/socket/README.ko.md:1116).

**(d) 최종 의견**

**추가하지 않는 권고 채택.** 이번 gap 판정에 필요한 행동 계약을 먼저 확정하십시오. 현재 테스트의 물리 귀속은 “직접 검증”으로 보고하지 않아야 합니다.

향후 운영 진단에서 반복적으로 필요해지면, 단일 `connection_id` 대신 **request admission과 reply 수신의 별도 진단 정보**를 다루는 독립 API 제안으로 검토할 수 있습니다. 그때도 reconnect fence나 target으로 사용하는 기능은 부여하지 않습니다.

**(e) Spec 문장 초안**

> REQUEST completion의 `completion_id`와 `user_context`는 operation correlation에 사용한다. 현재 public API는 request가 admission된 physical connection이나 reply가 수신된 physical connection의 ID를 반환하지 않는다. Request와 reply가 동일 physical connection을 사용한다는 보장은 없다. Monitor의 `connection_id` 및 event 관찰 순서만으로 개별 request의 물리 경로를 확정해서는 안 되며, reply token을 해석하여 이를 추출해서도 안 된다.

사용자에게는 이렇게 설명할 수 있습니다. **연결 종료 알림과 요청의 답변 완료는 서로 다른 일입니다.** Inproc은 연결이 끊겼다는 알림이면 충분하고, 네트워크 자원 해제를 뜻하는 추가 알림까지 맞출 필요는 없습니다. 재연결은 이전 자원을 정리하는 동안 시작할 수 있지만, 새 요청이 어느 연결을 선택할지와 기존 요청을 어떻게 끝낼지는 분명해야 합니다. HANDOVER는 새 연결로 교체하는 정책이지 모든 답변의 도착 보장은 아닙니다. 연결 번호를 새 API로 공개하기보다 이 동작 규칙부터 명확히 하는 것을 권합니다.

검토 완료, 요청한 파일 산출은 쓰기 권한 제한으로 미완료입니다.  
`EXIT:1`
hook: Stop
hook: Stop Completed
tokens used
94,278
**최종 의견: 1번 수정 채택, 2번 수정 채택, 3번 채택입니다.** 특히 2번의 “이전 pipe의 request는 자기 timeout으로 끝난다”는 기존 계약의 예외를 누락하므로 그대로 채택하면 안 됩니다.

`main` 작업 파일을 읽기 전용으로 검토했습니다. 종료 시 HEAD는 `09967a7d76`이며, 파일 변경·빌드·테스트는 실행하지 않았습니다. 아래 테스트 언급은 기존 소스·보고서를 검토한 결과입니다. 세션의 파일시스템 쓰기 금지로 진행 로그와 요약 파일은 생성·append하지 못했습니다.

**1. inproc peer close의 `CLOSED`: 발생 범위를 좁혀 수정 채택**

**(a) 계약 근거**

- [06-monitoring.ko.md:69](/home/hep7hep7/project/zlink/core/doc/spec/core/06-monitoring.ko.md:69): `connection_id`는 “하나의 물리적 transport 시도를 식별하는 진단·correlation 값”이며 reconnect fence가 아니다.
- [06-monitoring.ko.md:105](/home/hep7hep7/project/zlink/core/doc/spec/core/06-monitoring.ko.md:105): 같은 monitor의 commit 순서는 보장하지만, 서로 다른 connection의 wall-clock 순서는 보장하지 않는다. Queue는 bounded·lossy다.
- [socket/README.ko.md:609](/home/hep7hep7/project/zlink/core/doc/spec/core/socket/README.ko.md:609): socket close는 자원과 pending operation·unread completion을 정리한다. 이 계약은 모든 연결에서 `CLOSED` event를 받아야 한다고 규정하지 않는다.
- `CLOSED` enum은 있으나 transport별 발생 조건은 정의되어 있지 않다.

**(b) 권고안과 구현·Framework의 관계**

**inproc에 `CLOSED`를 추가하지 않는 방향은 타당합니다. 다만 “물리 transport(fd/listener) close”는 너무 넓습니다.**

현재 `event_closed` 호출은 TCP/IPC/WS/TLS의 **connecter와 listener**에 있습니다. 이미 연결된 공통 ASIO engine의 오류·단절 경로는 [`asio_engine.cpp:1938`](/home/hep7hep7/project/zlink/core/src/runtime/engine/asio/asio_engine.cpp:1938)에서 `DISCONNECTED`를 냅니다. 따라서 “모든 fd 해제마다 `CLOSED`”로 정의하면 기존 network transport에도 추가 구현이 필요합니다.

기존 identity 테스트도 연결 종료는 `READY → DISCONNECTED`, 실패한 접속 시도는 `CONNECT_DELAYED → CLOSED`로 대조합니다. [test_monitor_connection_identity.cpp:118](/home/hep7hep7/project/zlink/core/tests/integration/monitoring/test_monitor_connection_identity.cpp:118), [동일 파일:147](/home/hep7hep7/project/zlink/core/tests/integration/monitoring/test_monitor_connection_identity.cpp:147).

Framework는 inproc의 `CLOSED`를 필수로 기다리지는 않습니다.

- .NET ClientServer는 `Disconnected`와 `Closed`를 모두 처리합니다. [ZLinkClientServerClientRuntime.cs:1018](/home/hep7hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkClientServerClientRuntime.cs:1018).
- .NET Mesh는 `Disconnected`만 연결 후보 제거에 사용합니다. [ZLinkManagedMeshNode.cs:8643](/home/hep7hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:8643).
- Java Mesh는 두 event를 terminal 처리하고 endpoint별 후보 queue에서 항목을 제거합니다. `CLOSED`를 추가하면 같은 연결을 두 번 정리하지 않는지 검토해야 합니다. [ZLinkJavaRawMeshNode.java:6710](/home/hep7hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:6710), [동일 파일:7157](/home/hep7hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:7157).

별도로 WS/TLS connecter는 event마다 endpoint pair를 생성하는 코드가 남아 있습니다. 이는 같은 attempt의 ID 유지 계약과 불일치할 가능성이 있는 **기존 구현 문제**이며, inproc `CLOSED`의 정책 선택과 분리해야 합니다. [WS:438](/home/hep7hep7/project/zlink/core/src/runtime/transports/ws/asio_ws_connecter.cpp:438), [TLS:411](/home/hep7hep7/project/zlink/core/src/runtime/transports/tls/asio_tls_connecter.cpp:411). 이번 검토에서는 실행 확인하지 않았습니다.

**(c) 대안 비교**

| 선택 | 장점 | 단점·영향 범위 |
|---|---|---|
| connecter/listener 자원 해제 event로 한정 | 기존 발생 범위와 맞고 ABI·runtime 변경 불필요 | transport 공통 단절 신호는 `DISCONNECTED`라고 설명해야 함 |
| 모든 transport lifecycle 종료에 `CLOSED` | event 이름의 직관성이 높음 | inproc뿐 아니라 network engine도 검토·수정 필요. `DISCONNECTED`와 순서·중복·미연결 attempt 의미를 새로 정의해야 함. 모든 binding monitor 계약·Framework terminal 처리 영향 |

**(d) 최종 의견**

**수정 채택.** `CLOSED`는 connecter/listener가 소유한 OS transport 자원의 해제 통지로 한정하십시오. “inproc은 DISCONNECTED로 끝난다”도 **해당 연결의 단절 관찰**이라는 범위가 필요합니다. Connect intent가 살아 있으면 이후 자동 재연결은 계속됩니다.

**(e) Spec 문장 초안**

> `CLOSED`는 TCP·IPC·WS·TLS의 접속 시도 또는 listener가 소유한 OS transport handle을 닫았음을 알리는 event다. 이미 성립한 연결의 단절은 `DISCONNECTED`로 보고하며, 그 연결에 추가 `CLOSED`가 반드시 발생하는 것은 아니다. Inproc peer의 단절은 `DISCONNECTED`로 보고하고 `CLOSED`는 발생시키지 않는다. `DISCONNECTED`는 connect intent의 제거를 뜻하지 않으며 자동 재연결 여부는 connect/disconnect 계약을 따른다. Monitor event의 소비는 연결 종료나 재연결 진행의 전제 조건이 아니다.

---

**2. 즉시 disconnect→connect: overlap은 허용하되 request 결과를 일괄 timeout으로 고정하지 말 것**

**(a) 계약 근거**

현재 계약에는 서로 다른 종료 상황이 이미 구분되어 있습니다.

| 상황 | 기존 계약 |
|---|---|
| 같은 RID, 같은 방향의 중복 연결 | REJECT는 기존 pipe 유지, HANDOVER는 새 pipe가 인수 |
| 반대 방향 충돌에서 물러나는 pair | admit된 request를 즉시 `NOT_CONNECTED`로 종료. 자기 timeout까지 기다리지 않음 |
| admission 이후 일시적 물리 단절 | payload replay 없이 기존 correlation·timeout budget 유지 |
| endpoint 또는 logical RID 명시적 제거 | REQUEST는 `NOT_FOUND` |
| responder의 reply 경로 | 같은 logical RID의 현재 ready pipe 사용. 물리 단절만으로 reply token을 무효화하지 않음 |

근거: [socket/README.ko.md:159](/home/hep7hep7/project/zlink/core/doc/spec/core/socket/README.ko.md:159), [동일 파일:1059](/home/hep7hep7/project/zlink/core/doc/spec/core/socket/README.ko.md:1059), [동일 파일:1129](/home/hep7hep7/project/zlink/core/doc/spec/core/socket/README.ko.md:1129), [07-router.ko.md:291](/home/hep7hep7/project/zlink/core/doc/spec/core/socket/07-router.ko.md:291), [동일 파일:317](/home/hep7hep7/project/zlink/core/doc/spec/core/socket/07-router.ko.md:317).

[03-errors.ko.md:359](/home/hep7hep7/project/zlink/core/doc/spec/core/03-errors.ko.md:359)도 `TIMED_OUT`, `NOT_FOUND`, `NOT_CONNECTED`를 별개 결과로 정의합니다.

**(b) 권고안의 문제와 Framework 대조**

첫째, **monitor 소비와 물리 종료를 구분해야 합니다.** Event가 queue에 있지만 아직 읽히지 않은 상태와, 이전 연결이 실제로 종료 중인 상태는 다릅니다. Lossy monitor 소비를 Core connect의 진행 조건으로 삼으면 안 됩니다. Polling 계약의 내부 command 처리·lost-wake 보장도 그대로 유지해야 합니다. [05-polling.ko.md:73](/home/hep7hep7/project/zlink/core/doc/spec/core/05-polling.ko.md:73).

둘째, **기존 테스트는 “request가 이전 pipe로 갔다”를 직접 증명하지 않습니다.** Request는 disconnect→connect **이후** 제출됩니다. 따라서 disconnect 전에 admit된 pending request와 다른 사례입니다. 테스트 끝의 `reply_connection`은 opaque token 때문에 추론한 값이며, 성공한 reply만으로 request 송신 경로까지 같은 connection이었다고 입증할 수 없습니다. [test_socket_disconnect_progress_without_app_poll.cpp:447](/home/hep7hep7/project/zlink/core/tests/integration/test_socket_disconnect_progress_without_app_poll.cpp:447), [동일 파일:515](/home/hep7hep7/project/zlink/core/tests/integration/test_socket_disconnect_progress_without_app_poll.cpp:515).

REJECT에서 새 연결이 **상대편의 아직 남은 동일 RID 등록**과 충돌하는 상황도 구분해야 합니다. 이는 local requester가 이미 제거한 pipe를 다시 선택했다는 주장과 다릅니다.

셋째, **Core overlap 허용만으로 Framework 대기를 삭제할 수 없습니다.** 현재 Framework wire spec은 다음을 명시합니다.

> “그 endpoint의 close snapshot 또는 disconnect event를 받기 전에는 같은 endpoint에 새 connection을 만들지 않는다.”

[06-wire-protocol.ko.md:335](/home/hep7hep7/project/zlink/framework/doc/framework/common/spec/server/02-channel-transport/06-wire-protocol.ko.md:335).

실제 구현에도 차이가 있습니다.

- .NET ClientServer는 terminal 관찰을 기다리고, 그동안 별도 poller를 돌립니다. [ZLinkClientServerClientRuntime.cs:1640](/home/hep7hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkClientServerClientRuntime.cs:1640).
- Java ClientServer는 같은 lock 안에서 즉시 disconnect→connect합니다. [ZLinkChannelSocketRegistry.java:1091](/home/hep7hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelSocketRegistry.java:1091).
- .NET Channel/Mesh와 Java Channel의 ROUTER 구성에는 HANDOVER가 켜져 있습니다. [ZLinkChannelBundleFactory.cs:55](/home/hep7hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkChannelBundleFactory.cs:55), [ZLinkManagedMeshNode.cs:289](/home/hep7hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:289), [ZLinkJavaSocketOptions.java:23](/home/hep7hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaSocketOptions.java:23).

Java/.NET 차이는 언어 구조상 불가피한 것으로 확인되지 않았습니다. **현재 Framework 계약과 구현의 불일치로 별도 판정할 사항**입니다. Core의 command progress 수정은 두 번째 poller의 필요성을 없앨 수 있지만, Framework의 교체 관찰 규칙까지 자동으로 없애지는 않습니다.

**(c) 대안 비교**

| 선택 | 장점 | 단점·영향 범위 |
|---|---|---|
| 현재 OK/timeout 관찰을 그대로 계약화 | 당장 구현 비용이 작음 | 원인을 증명하지 않은 “old pipe 귀속”을 고정하고 기존 terminal 규칙을 침해할 위험 |
| 이전 physical terminal까지 Core에서 완전 직렬화 | 호출 순서 설명이 단순함 | local 종료와 remote 등록 제거 중 무엇을 기다리는지 추가 정의 필요. Core endpoint/session·inproc·모든 transport와 binding 호출 지연에 영향 |
| **local 연결 제거와 physical teardown을 분리** | 이전 자원 정리와 새 attempt는 겹칠 수 있고, 새 submit의 local 대상은 명확해짐 | local admission 경계의 구현 검증 필요. 상대편 RID 충돌은 여전히 정책으로 처리해야 함 |

**(d) 최종 의견**

**수정 채택.** 완전한 physical 종료 대기를 강제하기보다는, **disconnect 성공으로 제거한 local 연결을 후속 신규 submit이 다시 선택하지 않는 경계**를 명확히 하는 쪽을 권합니다. 이전 자원의 teardown과 새 attempt는 겹칠 수 있고, 상대편에 남은 동일 RID는 기존 REJECT/HANDOVER 계약으로 처리합니다.

이 local admission 경계는 이번 리뷰의 **설계 권고**이며 현재 구현이 모든 transport에서 만족한다고 검증한 것은 아닙니다. “코드 변경 없음”으로 확정해서는 안 됩니다.

HANDOVER는 동일 logical peer의 교체를 허용하려는 경우 권장할 수 있지만, 모든 request의 성공·무손실을 보장하는 옵션은 아닙니다. 중복 연결을 거부하려는 REJECT의 용도도 유지해야 합니다.

**(e) Spec 문장 초안**

> 성공한 `zlink_disconnect(endpoint)`는 해당 local 연결 등록과 자동 재연결 intent를 제거한다. 후속 신규 submit은 제거된 연결을 admission 대상으로 다시 선택하지 않는다. 관련 physical 자원의 비동기 종료와 이후 `zlink_connect(endpoint)`가 시작하는 새 연결 시도는 겹칠 수 있으며, 새 시도의 진행은 monitor event 소비를 기다리지 않는다. 상대편에 동일 RID의 기존 연결이 남아 있으면 RID duplicate policy를 적용한다. Connect 성공은 remote admission이나 request 전달 성공을 보장하지 않는다.
>
> Request 종료는 completion 계약을 따른다. 명시적 endpoint·logical RID 제거는 `NOT_FOUND`, 반대 방향 handover로 물러나는 pair의 request는 `NOT_CONNECTED`로 처리한다. 일시적 physical disconnect에서는 admission된 payload를 replay하지 않고 기존 correlation과 timeout budget을 유지한다. 유효 reply와 timeout 중 먼저 확정된 결과가 완료를 결정하며, physical disconnect만으로 reply token을 무효화하지 않는다.

Framework wire spec의 관찰 대기 조항을 바꾸려면 별도 계약 정합성 검토가 필요합니다. 해당 보호 문서는 이번 작업에서 변경하지 않았습니다.

---

**3. request/reply physical `connection_id` 공개 API: 추가하지 않는 권고 채택**

**(a) 계약 근거**

- [06-monitoring.ko.md:536](/home/hep7hep7/project/zlink/core/doc/spec/core/06-monitoring.ko.md:536): `connection_id`로 send·reply target을 지정하는 public API는 없다.
- [socket/README.ko.md:287](/home/hep7hep7/project/zlink/core/doc/spec/core/socket/README.ko.md:287): completion record는 operation의 `completion_id`, context, logical RID, 결과·payload를 제공한다.
- [동일 파일:1124](/home/hep7hep7/project/zlink/core/doc/spec/core/socket/README.ko.md:1124): `peer_rid`는 logical peer snapshot이며 reconnect 뒤 physical connection identity로 바뀌지 않는다.
- [07-router.ko.md:294](/home/hep7hep7/project/zlink/core/doc/spec/core/socket/07-router.ko.md:294): reply는 현재 ready Application 또는 Completion pipe를 사용한다.

**(b) 모순 여부**

추가하지 않는 결정은 기존 계약과 맞습니다. 다만 **“진단 필드 추가가 기존 spec상 금지되어 있다”는 뜻은 아닙니다.** 진단용 노출과 라우팅 capability 제공은 다릅니다.

문제는 “request/reply가 사용한 connection ID”가 단일 값으로 명확하지 않다는 점입니다.

- Request가 admission된 연결과 reply가 돌아온 연결은 다를 수 있습니다.
- ROUTER–ROUTER에서는 Application·Completion lane 자체가 다릅니다.
- Timeout에는 reply 수신 연결이 없습니다.
- WRITABLE은 아직 request가 admission되지 않은 대기 토큰입니다.
- ID는 현재 프로세스 범위이므로 remote ID와 직접 비교하는 계약도 아닙니다.

Framework의 physical fence 문제를 이 필드로 해결하려 하면 기존 계층 소유권을 다시 침해합니다. 필요한 동작 판단은 logical identity·admission·liveness와 Core completion 결과로 해야 합니다.

**(c) 대안 비교**

| 선택 | 장점 | 단점·영향 범위 |
|---|---|---|
| **현재 API 유지** | ABI·binding 변경 없음. operation과 physical connection의 책임 구분 유지 | 특정 request의 물리 경로를 public API만으로 직접 입증할 수 없음 |
| completion에 진단 필드 추가 | timeout·handover 분석과 회귀 검증에 도움 | 송신 admission ID·reply 수신 ID·lane·값 부재 의미를 구분해야 함. C layout, Core correlation 보관, C/C++/.NET/Java/Node/Rust/Python/Go binding 변환·계약·테스트 영향 |

Completion은 caller의 `struct_size`를 검증하므로, 필드 추가를 단순한 무비용 확장으로 볼 수 없습니다. [socket/README.ko.md:1116](/home/hep7hep7/project/zlink/core/doc/spec/core/socket/README.ko.md:1116).

**(d) 최종 의견**

**추가하지 않는 권고 채택.** 이번 gap 판정에 필요한 행동 계약을 먼저 확정하십시오. 현재 테스트의 물리 귀속은 “직접 검증”으로 보고하지 않아야 합니다.

향후 운영 진단에서 반복적으로 필요해지면, 단일 `connection_id` 대신 **request admission과 reply 수신의 별도 진단 정보**를 다루는 독립 API 제안으로 검토할 수 있습니다. 그때도 reconnect fence나 target으로 사용하는 기능은 부여하지 않습니다.

**(e) Spec 문장 초안**

> REQUEST completion의 `completion_id`와 `user_context`는 operation correlation에 사용한다. 현재 public API는 request가 admission된 physical connection이나 reply가 수신된 physical connection의 ID를 반환하지 않는다. Request와 reply가 동일 physical connection을 사용한다는 보장은 없다. Monitor의 `connection_id` 및 event 관찰 순서만으로 개별 request의 물리 경로를 확정해서는 안 되며, reply token을 해석하여 이를 추출해서도 안 된다.

사용자에게는 이렇게 설명할 수 있습니다. **연결 종료 알림과 요청의 답변 완료는 서로 다른 일입니다.** Inproc은 연결이 끊겼다는 알림이면 충분하고, 네트워크 자원 해제를 뜻하는 추가 알림까지 맞출 필요는 없습니다. 재연결은 이전 자원을 정리하는 동안 시작할 수 있지만, 새 요청이 어느 연결을 선택할지와 기존 요청을 어떻게 끝낼지는 분명해야 합니다. HANDOVER는 새 연결로 교체하는 정책이지 모든 답변의 도착 보장은 아닙니다. 연결 번호를 새 API로 공개하기보다 이 동작 규칙부터 명확히 하는 것을 권합니다.

검토 완료, 요청한 파일 산출은 쓰기 권한 제한으로 미완료입니다.  
`EXIT:1`
