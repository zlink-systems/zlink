[English](./framework-java-0.29.1.md) | [한국어](./framework-java-0.29.1.ko.md)

# ZLink Java·Kotlin Framework 0.29.1 릴리스 노트

Framework 0.29.1은 Java·Kotlin binding 1.17.0과 Core 1.17.0을 사용합니다.

## 결함 수정

- 2코어 환경에서 Java native socket monitor 대기가 virtual-thread carrier를 점유해 route 준비와 request 처리가 멈추던 문제를 수정했습니다. Stream·Channel monitor 실행자는 공용 backend owner가 선택하며, Stream·Channel과 ClientServer control의 native 대기는 platform thread에서 진행합니다. (#1567, #1568)
- Windows Kotlin GameQuest runner가 `close-replay.release`를 만들지 않아 client와 runner가 서로 기다리던 문제를 수정했습니다. (#1568)
- 같은 Instance Spot을 깨우는 operation이 활성화 도중에 도착하면 `Unavailable`·`stale_target`으로 실패하던 0.29.0 회귀를 수정했습니다. 뒤에 도착한 operation은 진행 중인 활성화에 합류하고, Ready 뒤 도착 순서대로 처리됩니다. (#1571)
- 같은 대상의 활성화에 합류한 operation이 부하에서 도착 순서와 다르게 처리될 수 있던 결함을 수정했습니다. (#1571)
- Owner node가 강제 종료된 Actor를 다시 `Create`·`GetOrCreate`하면 `location owner lease is unavailable`로 계속 실패하던 결함을 수정했습니다. Factory 등록에서 relocation을 끈(`DisableRelocation`) Actor type은 owner lease가 끝난 기존 record를 해제하고 새 incarnation을 만듭니다. Relocation 정책이 켜진 type은 계속 `Unavailable`을 반환합니다. (#1570)
- Java `Create`가 이미 있는 Actor에 대해 기존 Actor를 반환하던 동작을 다른 언어와 같이 `AlreadyExists`로 바꾸고, stable type이 다를 때의 오류 종류를 `TypeMismatch`로 맞췄습니다. (#1570)
- Actor 종료가 이미 수락한 handler turn이 끝나기 전에 dependency 정리를 시작하던 결함을 수정했습니다. (#1579)

## 설치

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.29.1")
    implementation("systems.zlink:zlink-http-client:0.29.1")
}
```

릴리스 태그는 [`framework-java/v0.29.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.29.1)입니다.
[한국어](./framework-java-0.29.1.ko.md) | [English](./framework-java-0.29.1.md)
