[English](./framework-java-0.31.0.md) | [한국어](./framework-java-0.31.0.ko.md)

# ZLink Java·Kotlin Framework 0.31.0 릴리스 노트

Framework 0.31.0은 Java·Kotlin binding 1.21.0과 Core 1.21.0을 사용합니다.

## 동작 변경

- message-flow 기록을 SLF4J provider로 냅니다. JUL handler만 구성한 application은 SLF4J→JUL binding(예: `org.slf4j:slf4j-jdk14`)을 추가해야 message-flow 기록을 계속 받습니다. provider가 없으면 기록을 만들지 않습니다. (#2758, #3246)

## 결함 수정

- Session handler가 대기 중이어도 연결 해제 정리가 진행됩니다. (#3187)
- explicit Close 접근과 Join lifecycle owner 처리를 유지하며, Spot Close는 Close operation의 deadline을 따르고 admission은 Spot incarnation에 맞게 처리됩니다. (#2672, #2758, #2849, #3130, #3178)
- Actor 요청·Join·teardown에서 취소, admission, 완료와 오류 결과를 보존하도록 수정했습니다. (#2287, #2426, #2507, #2530, #2738, #2758, #2790, #2830, #2878, #2815, #2978, #3071, #3132)
- Node와 Channel handler는 FIFO로 시작하며 앞 handler의 완료를 기다리지 않습니다. (#2758, #2859)
- Instance activation deadline 만료를 `DEADLINE_EXCEEDED`로 보존합니다. (#2242, #2471, #2879)
- Instance Spot send/request가 resolve한 Ready route를 사용합니다. (#1631, #2666)
- 같은 endpoint에 중복 연결하지 않으며 WebSocket connector는 OS proxy를 거치지 않습니다. (#1710, #2534, #2648, #2797)
- startup 취소·owner lease, Host 종료와 Spot closing lifecycle 처리를 수정했습니다. (#2233, #2286, #2289, #2470)
- User Spot retire 중 Continued 순서를 보존하고 relocation·Join 완료를 소유 lifecycle에 맞춰 처리합니다. (#2207, #2208, #2278, #2526, #2627, #2870)
- submit 전 미분류 실패를 원인과 함께 `InternalFailure`로 전달합니다. (#2360, #2380)
- Java binding 등록과 Session owner 신원 처리를 정리했습니다. (#2140, #2172, #2236)
- Actor typed reply를 STREAM 단일 part와 규격 Error payload로 인코딩합니다. (#2267, #2507)
- 연결이 준비되기 전에 들어온 remote request를 admission owner 하나가 판정합니다. 준비된 연결에서는 대기 구독 없이 바로 제출합니다. (#1710, #3257)
- `GetOrCreate`가 새 생성 capacity를 보기 전에 기존 authority를 확인합니다. capacity가 모두 쓰였을 때 이미 있는 User Spot을 다시 얻는 요청이 실패하던 문제를 수정했습니다. (#2799, #3242)

## 성능 개선

- send, request, receive, dispatch와 envelope 경로에서 중간 할당·복사를 줄였습니다. (#1640, #1642, #1648, #1673, #2126, #2159, #2165, #2168, #2182, #2185, #2216, #2252, #2254, #2272, #2281, #2321, #2352, #2354, #2608, #2770)
- receive/submit의 동기 대기와 receive pump의 반복 게시를 제거하고, deadline 기반 worker와 rotation 단위 permit 예약을 적용했습니다. (#1640, #1642, #1648, #2126, #2165, #2182, #2185)

## 진단과 계측

- runtime metrics의 관측 위치와 `zlink.spot.count`의 label·Spot 종류 값을 통일했습니다. (#2149, #2189, #2190, #2263, #2886)
- Channel 수신·handler tracing을 보완하고 stale Actor 대상을 진단하며 telemetry provider 전달을 격리했습니다. (#2412, #2799, #3150, #3181)
- Errors 진단 레코드에서 Flow 정보를 제외했습니다. (#3168)

## 예제 수정

- Bingo sample은 늦게 합류한 관찰자에게 방의 현재 상태와 확정 reward를 전달하고, diagnostics level을 설정에서 사용합니다. (#2208, #2224, #2427)

## 알려진 제한

- 시험 코드의 private 멤버 접근을 공개 경로로 옮기는 작업을 Node·Java·.NET에서 9차까지 진행했습니다. 남은 범위는 다음 릴리스에서 이어 갑니다. 제품 동작에는 영향이 없습니다. (#2309)
- Windows에서 mailbox signaler가 AF_UNIX datagram 실패 뒤 stream으로 전환하면 socket마다 임시 경로를 만들고 지우는 데 약 2.5ms가 듭니다. 수정은 다음 Core 릴리스에 포함됩니다. (#3053)

## 설치

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.31.0")
    implementation("systems.zlink:zlink-http-client:0.31.0")
}
```

릴리스 태그는 [`framework-java/v0.31.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.31.0)입니다.
