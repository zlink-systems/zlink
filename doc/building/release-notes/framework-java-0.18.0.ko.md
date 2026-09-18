[English](./framework-java-0.18.0.md) | [한국어](./framework-java-0.18.0.ko.md)

# ZLink Java Framework 0.18.0 릴리스 노트

Framework 0.18.0는 binding 1.2.0과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

Stream connector의 공개 표면이 바뀝니다.

- 표준 예외 대신 오류 코드를 담은 `ZLinkStreamException`을 던집니다. 표준 예외는 코드를 담을 자리가 없어 호출자가 `ValidationFailed`와 `ConfigurationError`를 구분하지 못했습니다.
- `ZLinkStreamConnectorOptions`는 구성 요소 21개의 record입니다. **자리 인자로 만들면 구성 요소가 늘 때마다 깨집니다.** `createDefault`에서 파생시키십시오.

## 공통 변경

- Runtime descriptor 변경의 게시 시점을 스펙에 적었습니다. 조율은 요청하는 쪽이 하고, 받는 쪽은 요청에 제약을 두지 않습니다. (#546)
- 원격 생성 예약 record의 `requestContentReference` 문법에서 checksum 구간을 없앰습니다. 예약은 별도의 record가 아니라 descriptor record의 상태입니다. (#559)
- 샘플은 한 번에 하나씩 실행합니다. 언어별 집계 러너를 없애고, 실행 방법을 공통 sample 문서가 소유하도록 했습니다. (#585)
- 언어별 e2e 시나리오 스위트를 걷어냈습니다. 크로스 언어 e2e는 유지합니다. (#541)

## 수정

- ActorClient의 message-follow 시험이 전체 실행에서 이따금 `one-way route is not connected`로 실패하던 것을 고쳤습니다. 송신 경로가 `VALIDATING_PREVIOUSLY_READY`를 허용하지 않아 생긴 경합입니다. (#533)
- 예약 record의 inline-v1 참조에 CRC32C 구간이 없어 다른 언어의 예약을 모두 거절하던 것을 고쳤습니다. (#559)

## 설치

```kotlin
implementation("systems.zlink:zlink-framework-core:0.18.0")
```

릴리스 태그는 [`framework-java/v0.18.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.18.0)입니다.
