[English](./framework-java-0.30.2.md) | [한국어](./framework-java-0.30.2.ko.md)

# ZLink Java·Kotlin Framework 0.30.2 릴리스 노트

Framework 0.30.2은 Java·Kotlin binding 1.20.0과 Core 1.20.0을 사용합니다.

## 호환성 변경

- `@ZLinkSpotSubscription`에 `channelName`이 필수가 되었습니다. Spot 구독은 channel과 topic의 쌍으로 식별되며, 이전에는 channel이 catalog와 dispatch에서 사라졌습니다. 구독하는 channel 이름을 함께 적습니다. 같은 동작을 등록 API `addSubscribe(handlerType, channelName, topic)`으로도 씁니다. (#1783)

  ```java
  // 0.30.1
  @ZLinkSpotSubscription(topic = "zone.border")
  // 0.30.2
  @ZLinkSpotSubscription(channelName = "zoneworld.zones", topic = "zone.border")
  ```

## 변경

- request admission과 reply 완료를 구분해 수락을 확인할 수 있습니다. deferred Join 완료와 HTTP handler의 Yield가 실행 turn과 올바르게 이어집니다. (#2065, #2044, #1916, #1912)
- 1ms 간격 deadline sweep을 deadline 기반 worker로 바꿔 불필요한 주기 작업을 없앴습니다. (#1820, #1846, #2090, #2094, #2185)
- relocation·Session·Join 과정에서 owner identity, source request owner와 capacity metric 관측 시점을 일관되게 유지합니다. (#2102, #2151, #2285, #1999, #1912)
- per-message 조회와 completion 처리를 줄이고 runtime metric label과 lifecycle을 계약에 맞췄습니다. (#1820, #1846, #2090, #2094, #2189)

## 결함 수정

- Join completion, Actor 종료, host drain이 현재 execution turn을 다시 기다리던 교착·정체를 수정했습니다. (#2044, #2122, #2290, #1978, #2248)
- relocation 중 Session seal·route 교체·binding 종료 순서가 맞지 않아 요청이나 종료가 멈추던 문제를 수정했습니다. (#1893, #1913, #1915, #1955, #1921, #1922)
- 시작 시 lease claim과 Ready timer가 지연되거나 이미 완료된 상태를 잘못 관측하던 문제를 수정했습니다. (#1880, #1897, #2100, #2288)
- source descriptor가 없는 request는 Unavailable로 끝나고, Session command는 저장된 owner identity를 사용합니다. (#2140, #2102, #2151)
- target Join lifecycle 실패의 원래 예외를 error reporter에 기록합니다. (#2459)
- Bingo 관찰자가 Join 뒤 방의 상태와 확정 reward를 조회하도록 sample을 수정했습니다. (#2427)
- server drain으로 닫힌 STREAM 연결의 close reason을 스펙 값 `server_shutdown`으로 기록합니다. (#2189)
- 성공한 startup을 stop 뒤 취소로 다시 판정해 Location runtime 정리가 구현되지 않은 store 연산을 호출하던 문제를 수정했습니다. (#2466)
- Core 1.20.0은 handshake 중 completion poller를 등록할 때 임시 transport owner의 executor가 멈추던 문제를 수정했습니다. (#2504)
- Actor reply를 STREAM frame 한 개로 보내고 오류 payload를 스펙의 `{code, message}` 형식으로 맞췄습니다. 네 언어가 같은 공유 vector로 검증합니다. (#2507)
- Join 콜백을 Spot lifecycle turn에서 실행하고, turn 복원과 teardown owner 판정 결함을 수정했습니다. (#2507)

## 설치

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.30.2")
    implementation("systems.zlink:zlink-http-client:0.30.2")
}
```

릴리스 태그는 [`framework-java/v0.30.2`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.30.2)입니다.
[한국어](./framework-java-0.30.2.ko.md) | [English](./framework-java-0.30.2.md)

