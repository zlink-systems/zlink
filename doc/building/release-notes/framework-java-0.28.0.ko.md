[English](./framework-java-0.28.0.md) | [한국어](./framework-java-0.28.0.ko.md)

# ZLink Java·Kotlin Framework 0.28.0 릴리스 노트

Framework 0.28.0은 Java·Kotlin binding 1.15.0과 Core 1.15.0을 사용합니다. (#1460)

## 계약 변경

- **One-way send에는 시간 상한과 caller cancellation이 없습니다.** RouteMesh node·Channel, Spot, Actor, ClientServer, bound session·session Actor relay의 send, commit된 Logical Multicast의 target별 제출, STREAM send·reply가 대상입니다. 대기는 capacity 회복 뒤 admission(정상 완료), 대기 중 route 제거(`Unavailable`), socket close·runtime shutdown(`ShuttingDown`)으로만 끝납니다. Core 1.15.0의 대기 토큰에는 기한이 없고 binding cancellation은 caller의 대기만 끝내므로, send를 시간이나 취소로 끝내면 "보내지 않았다"는 결과 뒤에 message가 나갈 수 있기 때문입니다. 전송 결과가 필요하면 request를 사용합니다. (#1461)
- **Request timeout은 outbound admission과 reply 대기를 함께 제한합니다.** Timeout이나 cancellation으로 caller의 대기가 끝나도 이미 시작한 binding 재제출로 request가 나중에 전송될 수 있으며, 그 reply는 폐기합니다. (#1461)
- **Classic fanout publisher만 send timeout을 사용합니다.** 값 규칙과 1초 기본값은 그대로입니다. (#1461)
- Missing Instance Spot에 대한 one-way cold activation의 activation deadline은 send 제출 시작 시각에 source MeshNode의 기본 request timeout을 더한 값이며, target의 activation 작업에만 적용하고 caller의 send 대기를 끝내지 않습니다. (#1461)
- Actor·User Spot 생성 전이(Reserve·Commit·Abort)는 target owner lease와 처음 읽은 target MeshNode descriptor의 StoreVersion을 함께 검증합니다. 이미 수락한 생성의 완료는 target이 Serving이 아니어도 진행합니다. (#1432)

## 호환성에 영향을 주는 변경

- `ZLinkClientServerChannelClientBuilder.setSendTimeout`, `ZLinkMeshNodeSocketConfig.sendTimeout`·`setSendTimeout`, `ZLinkSessionSendCall.timeout`, `ZLinkKotlinSessionSendCall.timeout`을 제거했습니다. `FanoutChannelBuilder.setSendTimeout`은 유지합니다. Send stage의 `cancel(false)`와 Kotlin coroutine cancellation은 caller의 대기만 끝내고 send는 계속합니다. (#1461)

## 결함 수정

- Ready commit이 공유 capacity conflict를 만나면 자격을 다시 확인해 요청을 재구성합니다(GameQuest owner 경합). (#1464)
- examples 미러: Java·Kotlin ZoneWorld Windows runner가 미러에서 공용 프로세스 스크립트를 찾도록 고쳤습니다. (#1035)

## 설치

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.28.0")
    implementation("systems.zlink:zlink-http-client:0.28.0")
}
```

릴리스 태그는 [`framework-java/v0.28.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.28.0)입니다.
