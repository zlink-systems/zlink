[English](./framework-java-0.34.0.md) | [한국어](./framework-java-0.34.0.ko.md)

# ZLink Java·Kotlin Framework 0.34.0 릴리스 노트

Framework 0.34.0은 Java binding 1.26.0과 Core 1.26.0을 사용합니다.

## 호환성 변경

- 0.34.0은 relocation 진행 상태를 authority record의 `aggregate.relocation` field에 기록합니다. 0.34.0으로 올리기 전에 진행 중인 relocation을 끝내고, 이 field가 남은 record가 있는 동안에는 0.33.x로 되돌리지 마십시오. 0.33.x는 이 field를 읽지 않습니다. (Location runtime §3.4, #4718, #4730)
- 0.34.0은 언어 간 aggregate relocation(SpotWide User Spot과 marker가 없는 Direct Join)을 지원하지 않습니다. 해당 relocation은 같은 언어의 노드 사이에서만 사용하십시오. 네 언어가 공유하는 §8.3 저장 형식으로의 전환은 0.35.0에 예정되어 있습니다. (#5354, #5422)

## 동작 변경

- 만료된 owner의 release wait를 authority record마다 따로 재지 않고 owner token마다 한 번 측정합니다. (#5302, #5349)
- relocation 진행 상태를 authority record의 `aggregate.relocation` JSON 필드에 기록합니다. (#4718, #5089)
- Instance Spot one-way send를 target activation deadline 없이 배치합니다. 배치 후보가 없으면 Unavailable을 반환하기 전에 descriptor를 한 번 새로 읽습니다. (#5312, #5380, #5365)

## 결함 수정

- mesh catch-up UPDATE를 ADMIT 수락 뒤에 보내고 connector의 공유 Netty event loop를 connect deadline 전에 준비합니다. (#5203, #5215)
- queue sequence admission이 실패해도 Yield한 turn의 terminal 결과를 해당 gate turn에서 전달합니다. (#5093, #5159)
- STREAM bind에서 Actor ID는 같지만 ObjectGeneration이 다르면 InvalidOperation을 반환합니다. route가 없거나 오래된 경우에는 Unavailable을 반환합니다. (#4774, #5003)
- relocation target은 target을 준비하고 소유권을 확정하기 전에 PREPARE coordinator fence를 현재 authority와 비교합니다. (#4762, #4779)
- Actor와 User Spot 생성 기한이 만료되면 DeadlineExceeded를 반환하며 호출자 취소와 구분합니다. (#5194, #5210)
- STREAM close는 같은 binding에 먼저 제출된 reply와 push가 끝난 뒤 수행합니다. Stream Connector는 연결 종료 원인을 한 번 전달합니다. (#4889, #4986, #4888, #5193)

## 성능 개선

- request·reply header를 최종 frame에 직접 쓰고 고정 header bytes와 heartbeat frame을 재사용합니다. (#4872, #5333)
- SpotWide Actor 대기열 소비 시 공유 gate 안에서 Actor lock을 다시 잡지 않습니다. (#5090, #5144)

## 설치

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.34.0")
    implementation("systems.zlink:zlink-http-client:0.34.0")
}
```

릴리스 태그는 [`framework-java/v0.34.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.34.0)입니다.
