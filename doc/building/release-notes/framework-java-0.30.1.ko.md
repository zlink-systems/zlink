[English](./framework-java-0.30.1.md) | [한국어](./framework-java-0.30.1.ko.md)

# ZLink Java·Kotlin Framework 0.30.1 릴리스 노트

Framework 0.30.1은 Java·Kotlin binding 1.19.1과 Core 1.19.0을 사용합니다.

## 변경

- 서버 Framework의 라이선스가 zlink License 1.0으로 바뀌며, 1.0.0 전 버전은 키나 유료 라이선스 없이 머신 수와 관계없이 무료입니다. Client connector·공용 client 코드·sample·tutorial·quickstart와 HTTP client는 Apache-2.0이며, 0.30.0 이하 릴리스의 라이선스는 바뀌지 않습니다. (#1681)
- 공개 API 추가: Java request 결과 `ZLinkRequestStage<TReply>.admitted()`와 Kotlin `submit()`으로 reply와 별도로 source-local 수락을 기다릴 수 있습니다. 수락 전에 실패하면 admission과 reply가 같은 원인으로 실패합니다. (#1682)
- Handler 메서드·인자·Kotlin suspend 호출 준비와 Protobuf parser 조회를 등록 시점에 수행하여 메시지마다 반복하던 reflection과 metadata 조회를 줄였습니다. User Spot placement가 준비 상태 전환 중 사용 가능한 target을 잘못 거부하던 결함도 수정했습니다. (#1657)

## 결함 수정

- 잘못된 envelope를 받은 request가 언어에 따라 오류 응답 없이 끝나거나 다르게 처리되던 결함을 수정했습니다. Header가 문법에 맞는 JSON object이고 유효한 correlation ID와 reply token이 있으면 오류 응답을 보내며, reply 경로를 확인할 수 없으면 응답 없이 관측 정보에 기록합니다. (#1671)
- Spot Configure의 잘못된 handler·subscription 등록을 activation의 Ready 전에 `InvalidOperation`으로 거부하도록 네 언어의 오류 분류를 맞췄습니다. (#1715)
- Relocation이 전달하는 새 owner generation을 Store CAS 결과와 일치시키고 sealed Session binding을 함께 전달합니다. 이동 후 Session route를 확정할 때 서로 다른 generation이나 누락된 binding 상태를 사용하던 경로를 수정했습니다. (#1807)
- HWM을 명시하지 않아도 Framework 고정값이나 무제한 값을 Core에 전달하던 결함을 수정했습니다. 명시적 override만 적용하고 나머지는 Core 기본값을 사용합니다. (#1684)
- 서버와 Stream Connector의 기본 typed JSON이 enum 숫자 ordinal, BOM, 비정규 64-bit 정수 문자열이나 비유한 실수를 받아들이던 결함을 수정했습니다. Java와 Kotlin이 공통 `framework-json-v1` 검증을 사용합니다. (#1703, #1714)
- Session 물리 종료 뒤 불필요한 Actor unbound를 보내거나 수신 pump가 자기 종료를 기다려 shutdown이 실패·정지하던 결함을 수정하여, remote binding은 close 시 퇴역하고 resource close는 deadline 뒤에도 순서대로 완료합니다. 이전 Session의 disconnect callback이 대기 중이어도 새 binding의 relay를 보존합니다. (#1708, #1810, #1828)
- Redis 같은 비동기 Location Store를 쓰면 User Spot close와 `onClosing`이 서로 기다려 끝나지 않던 결함을 수정했습니다. Authority 조회 뒤 원래 Spot execution turn에서 close를 계속합니다. (#1727)
- 열린 generic handler 정의가 scan 후보에 포함되어 node 시작이 실패하던 결함을 수정했습니다. 인스턴스를 만들 수 없는 후보는 제외하며 명시적인 잘못된 등록은 계속 거부합니다. (#1736)
- 빈 application reply를 가진 cross-node deferred Join이 `DataLost`로 거부되던 결함을 수정했습니다. 빈 reply의 metadata를 만들지 않고 recovery에서도 허용하며, target에서 reply 형식을 중복 검사하지 않습니다. (#1737, #1746)
- Local request나 이동 중 Actor send가 owner queue에 수락되기 전에 admission 성공을 알리던 결함을 수정했습니다. Caller admission continuation은 owner 내부 관찰과 분리해 새 turn에서 실행하며, queue 수락 전 실패는 admission과 reply에 같은 원인으로 전달합니다. (#1759, #1764, #1810)
- 느린 status observer나 부족한 common-pool worker 때문에 다른 observer 전달·topology 조회·lane 완료가 멈추던 결함을 수정했습니다. Observer 전달과 infrastructure 완료를 분리하고, topology projection은 source map 잠금 밖에서 수집합니다. (#1804, #1806, #1810)
- 작은 queue capacity에서 remote logical multicast가 수신 permit을 보유한 채 child permit을 기다려 멈추던 결함을 수정했습니다. 첫 child에 수신 permit을 이전하고 나머지는 같은 source가 확보하며, 송신 snapshot이 고른 target을 수신 측에서 다시 제외하지 않습니다. (#1805)
- Application Entry 타입이 없는 Object Server에서 Session bind 확인이 처리되지 않던 결함을 수정했습니다. 내부 Entry dispatcher를 항상 설치하고, 비동기 초기화가 끝난 뒤 공개하며 초기화 실패는 startup 실패로 보고합니다. (#1810)
- Java binding 1.19.1을 사용합니다. Virtual thread에서 닫은 Message의 native slot이 종료한 thread의 pool에 남아 재사용되지 않던 누수를 수정한 버전입니다. (#1811)
- Spot reply frame의 이중 해제·누락, unbind ack 뒤 퇴역한 binding으로의 push, Instance Spot 합류 결과가 Ready 게시보다 먼저 완료되던 경합을 수정했습니다. (#1827)
- 초기 owner lease claim 완료 시각을 기준으로 첫 heartbeat를 예약해 갱신이 늦어지던 결함을 수정했습니다. Claim 시작 시각부터 interval을 계산합니다. (#1830)
- Relocation source의 `safeToShutdown` 조회가 true여도 관찰 stream에는 변경이 게시되지 않던 결함을 수정했습니다. 조회와 관찰은 같은 status snapshot을 사용하며, `findSpot`도 authority generation 대신 현재 Spot generation을 반환합니다. (#1833)
- Target이 commit 직후 종료해 authority가 `Missing`이면 source가 재조회를 반복하고 host close가 멈추던 결함을 수정했습니다. Preserve를 시도하지 않고 expired-owner terminal로 종료합니다. (#1847)
- Route 교체 중 cross-node Join과 relocation frame 유실로 대기하던 결함을 수정하여, 다른 선택 route를 관찰하면 ready 상태에서 durable request와 batch·cutover를 다시 제출합니다. 선택 route의 handshake만 수락하고 ADMIT까지 같은 HELLO snapshot을 재전송한 뒤 변경은 UPDATE로 게시하며, 기존 route·manual intent가 있는 상대에게 자동 중복 연결을 만들지 않습니다. (#1852, #1874)
- RID로 수동 연결한 Ready peer의 lifecycle generation을 0으로 보고해 Spot placement와 Kotlin TicTacToe 요청이 시간 초과되던 결함을 수정했습니다. Peer의 generation·revision·channel 수는 admitted topology descriptor에서 조회합니다. (#1874)
- 진행 중인 deferred Join의 admission을 진행 상태와 무관한 1초 timer가 지워 target이 `target rejected canonical relocation: 17`·`35`(`DataLost`)로 거절하던 결함을 수정했습니다. PREPARE를 수락한 target attempt가 준비 자원의 수명을 인수해 그 attempt의 종료 경로에서만 정리하며, 준비만 된 항목은 등록 기본 timeout 뒤 만료됩니다. Admission이 사라진 recovery는 `Unavailable`로 보고합니다. (#1729)
- Instance Spot Close가 queue로 넘어가는 중인 수신 frame을 먼저 해제해, 같은 메시지를 새 incarnation이 처리할 때 `HANDLER_MISSING for packet ''`로 실패하던 결함을 수정했습니다. 대기 중인 frame은 execution queue가 소유합니다. Channel 생성 전에 기동이 실패하면 rollback이 원래 예외에 NullPointerException을 덧붙이던 결함도 수정했습니다. (#1745)

## 설치

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.30.1")
    implementation("systems.zlink:zlink-http-client:0.30.1")
}
```

릴리스 태그는 [`framework-java/v0.30.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.30.1)입니다.
[한국어](./framework-java-0.30.1.ko.md) | [English](./framework-java-0.30.1.md)
