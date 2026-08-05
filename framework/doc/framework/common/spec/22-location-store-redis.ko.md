---
title: "Location Store provider SPI와 공식 Redis 구현"
---

# Location Store provider SPI와 공식 Redis 구현

[스펙 목차](README.ko.md) · [이전: Location runtime](21-location-runtime.ko.md) · [다음: Relocation Store provider SPI와 공식 Redis 구현](23-relocation-store-redis.ko.md)

> **이 장이 정의하는 것** — Location Store provider가 지켜야 하는 공개 SPI(조건부
> commit, 페이지 제한 snapshot).


## 1. 범위와 독자

이 문서는 Location Store provider를 구현하는 개발자가 지켜야 하는 공개 SPI를 정의한다. Provider는
Framework가 만든 opaque key와 bytes를 저장한다. 여러 key의 조건 검사와 변경은 하나의 commit으로 적용한다.
복구용 snapshot은 page 크기를 제한해 제공한다.

Provider가 Actor·Spot authority, owner lease, placement reservation, aggregate commit과 relocation phase의
의미를 알아야 하는 것은 아니다. Framework가 이 SPI를 사용해 해당 상태를 구성하는 방법과 Store 등록 조건은
[Location runtime](21-location-runtime.ko.md)이 소유한다. 이 문서는 그 동작을 반복하지 않는다.

Application은 이 SPI의 operation을 직접 호출하지 않는다. Provider package만 SPI를 구현하며 Framework가
등록된 instance를 사용한다.

## 2. 공개 SPI의 책임

Location Store SPI는 다음 세 operation군만 제공한다.

| Operation군 | Provider가 보장하는 결과 |
|---|---|
| Exact read | 하나의 opaque key에 대해 현재 bytes, provider version, optional expiry와 `StoreNow`를 같은 관측으로 반환한다. |
| Conditional atomic batch | 모든 condition이 참일 때만 모든 mutation을 하나의 commit으로 적용한다. |
| Snapshot scan | 첫 page에서 고정한 snapshot을 정해진 page 크기와 opaque cursor로 이어 읽는다. |

SPI type과 interface는 기본 Framework API와 분리된 provider abstraction package 또는 module이 소유한다.
기본 Framework package는 abstraction에 의존하지만 Store operation을 application API로 다시 노출하지 않는다.
외부 provider는 application·Actor·Spot package에 의존하지 않고 abstraction만 구현할 수 있어야 한다.

Descriptor, authority, reservation, capacity, aggregate, lease와 change-stamp별 public method나 DTO를 추가하지
않는다. Redis command, key layout, script와 private record encoding도 공개 SPI에 노출하지 않는다.

다음 .NET 발췌는 공통 SPI의 최소 모양을 보여준다. 정식 선언은
[.NET exact interface](server/languages/dotnet/interfaces/08-authority-relocation.ko.md)에 있다.

```csharp
public interface IZLinkLocationStore
{
    ValueTask<ZLinkStoreReadResult> ReadAsync(
        ZLinkStoreKey key,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkStoreWriteResult> WriteAsync(
        ZLinkStoreWriteRequest request,
        CancellationToken cancellationToken = default);

    ValueTask<ZLinkStoreScanResult> ScanAsync(
        ZLinkStoreScanRequest request,
        CancellationToken cancellationToken = default);
}
```

다른 언어의 정식 모양은
[Java](server/languages/java/interfaces/location-maintenance.ko.md),
[Kotlin](server/languages/kotlin/interfaces/location-maintenance.ko.md),
[Node.js](server/languages/node/interfaces/08-location-maintenance.ko.md)와
[C++](server/languages/cpp/interfaces/07-location-store.ko.md) exact interface를 따른다.
두 Store의 등록 예제는 [Location runtime](21-location-runtime.ko.md#13-등록-조건과-수명)에만 둔다.

## 3. Key, value, version과 clock

| 항목 | 계약 |
|---|---|
| Key | Framework가 발급하는 opaque UTF-8 `1..1024` bytes다. Case-sensitive exact match를 사용하며 normalization과 case folding을 적용하지 않는다. |
| Value | 최대 1 MiB인 bytes다. Commit 뒤에는 해당 version이 교체되거나 삭제될 때까지 변경되지 않는다. Expiry가 없으면 explicit delete까지 유지한다. |
| Version | Provider가 발급하는 opaque UTF-8 `1..4096` bytes다. Framework는 값의 크기나 내부 구성을 해석하지 않는다. |
| `StoreNow` | Read, commit과 scan page가 기준으로 삼는 provider wall clock이다. TTL과 expiry correctness는 이 시각만 사용한다. |

Exact read는 `Missing(StoreNow)` 또는
`Found(bytes, version, optional expiry, StoreNow)`를 반환한다. 만료된 value는 `Missing`으로 반환한다.
Provider는 consumer가 read result를 사용하는 동안 bytes를 변경하거나 다른 result buffer에 재사용하지 않는다.

Framework domain generation은 provider version과 다르다. Provider는 domain counter를 해석하거나 별도
generation API를 제공하지 않는다.

## 4. Conditional atomic batch

Write request는 condition 집합과 mutation 집합으로 구성한다.

- `Missing(key)`는 key가 없거나 만료된 경우에만 참이다.
- `Version(key, expected)`는 current version이 exact match인 경우에만 참이다.
- `Put(key, bytes, optional retention)`은 새 opaque version을 발급한다.
- `Delete(key)`는 key를 제거한다.

Provider는 모든 condition을 먼저 검사한다. 모두 참일 때만 모든 mutation을 하나의 commit으로 적용한다.
다른 caller는 commit의 중간 상태를 관찰할 수 없다. Condition 하나라도 거짓이면 `Conflict`를 반환하고
mutation과 version 증가는 0이다. `Conflict`는 실패한 condition이나 current value를 반환하지 않는다.

Batch에는 다음 bound를 적용한다.

- Condition과 mutation에 나타난 unique key 합계는 최대 2,048개다.
- Encoded request는 최대 4 MiB다.
- 같은 key를 condition 안에서 또는 mutation 안에서 두 번 사용하지 않는다.
- `Applied`는 각 `Put`의 opaque version과 commit에서 관측한 하나의 `StoreNow`를 반환한다.

User Spot participant 전체를 이 batch 하나에 넣지 않는다. Framework는 최대 1,024개
항목과 encoded 1 MiB로 제한한 immutable inventory chunk를 미리 저장한다. 마지막
batch에는 aggregate authority, inventory root·count·digest와 capacity counter처럼
공개 시점에 함께 바뀌어야 하는 작은 record만 넣는다.

따라서 한 User Spot에 속할 수 있는 Actor 총수는 batch의 2,048-key 제한으로 정하지
않는다. Provider는 inventory chunk, participant와 aggregate의 의미를 해석하지 않는다.

## 5. 크기를 제한한 snapshot scan

Snapshot scan은 복구와 maintenance가 Framework record를 제한된 크기로 읽게 한다.

- Prefix는 UTF-8 `0..1024` bytes이며 key와 같은 exact comparison을 사용한다.
- 첫 page 요청에는 cursor가 없다. Provider는 크기를 제한한 snapshot을 만들고 다음 page가 있으면 opaque cursor를
  반환한다.
- 같은 cursor의 다음 page는 처음 고정한 snapshot만 읽는다.
- Page limit은 `1..1000`이고 encoded page 크기는 최대 4 MiB다.
- Cursor는 opaque UTF-8 `1..4096` bytes다.
- Snapshot이 더 이상 존재하지 않거나 cursor가 유효하지 않으면 `Expired`를 반환한다.

Framework는 `Expired`를 받으면 이전 page 결과를 버리고 첫 page부터 다시 읽는다. Scan item은 복구 후보일
뿐이므로 mutation 전에 exact read와 expected version condition으로 다시 확인한다.

Cursor encoding, snapshot 보존 구조와 Redis `SCAN` 사용 여부는 provider implementation detail이다.

## 6. Cancellation, 결과 유실과 오류

Operation 시작 전 cancellation은 I/O와 commit 시작을 막는다. Operation 시작 뒤 cancellation, timeout 또는
transport error가 발생하면 commit 여부가 불명확할 수 있다. Provider는 이를 성공이나 `Conflict`로 추정하지
않는다. Framework가 exact read와 expected version으로 결과를 재구성할 수 있어야 한다.

입력 bound 위반과 같은 key를 condition 또는 mutation 안에서 중복 지정한 caller 오류는 언어별 argument
validation error다. `Missing`, `Conflict`와 `Expired`는 정상적인 closed result다. Provider-specific failure는
Framework가 Store failure로 분류할 수 있어야 하지만 Redis command, key layout이나 script 정보를 application
public API에 노출하지 않는다.

Input bytes는 asynchronous operation이 끝날 때까지 변경되지 않아야 한다. Provider가 그 뒤에도 보관하려면
복사한다. Success result의 bytes는 consumer가 사용하는 동안 stable해야 한다.

## 7. 등록, 수명과 공식 Redis provider

Provider instance의 등록 조건과 Framework root의 소유권은
[Location runtime의 Store 등록](21-location-runtime.ko.md#1-범위와-책임)을 따른다. Framework가 instance
수명을 소유하는 구성에서는 Store를 사용하는 runtime과 background operation이 모두 끝난 뒤 정확히 한 번
dispose한다. 여러 Store가 물리 connection을 공유할 때 중복 dispose를 막는 책임은 provider 구현에 있다.

공식 Redis extension package는 언어별 naming convention에 맞는 `RedisLocationStore` 구현을 제공한다.
공개 options는 instance 생성에 필요한 connection, key namespace와 operation timeout으로 제한한다.

다음 항목은 Redis provider implementation detail이며 public contract가 아니다.

- Redis key와 hash tag layout
- HASH·SET·ZSET 선택
- Lua script와 transaction 분할 방식
- Private record encoding과 schema marker
- Connection lease, retry와 snapshot cursor 구현
- Change stamp와 polling 최적화

Redis provider도 §4의 generic atomic batch와 §5의 snapshot scan을 그대로 지원해야 한다. Domain별 Redis
method, descriptor·authority DTO와 change-stamp capability interface를 공개하지 않는다.

Location Store와 Relocation Store는 같은 Redis deployment에서 서로 다른 key namespace를 사용할 수도 있고
물리적으로 분리할 수도 있다. Correctness는 connection 공유나 cross-store Redis transaction에 의존하지 않는다.

## 8. Contract test

- Exact read가 bytes, version, optional expiry와 `StoreNow`를 같은 관측으로 반환한다.
- 만료된 value는 provider clock 기준 `Missing`이고 durable value는 explicit delete 전까지 유지된다.
- Condition 하나가 실패하면 모든 mutation과 version 증가가 0이다.
- 최대 2,048 unique key와 encoded 4 MiB request가 하나의 atomic commit으로 적용된다.
- Scan page가 같은 snapshot을 사용하며 snapshot 또는 cursor가 유효하지 않으면 `Expired`를 반환한다.
- Cursor는 4,096 bytes까지 opaque하게 왕복하고 page는 1,000 item·4 MiB bound를 지킨다.
- Cancellation이나 결과 유실 뒤 exact read와 version으로 commit 여부를 재구성할 수 있다.
- Redis provider public declaration에 authority·reservation·aggregate DTO, script와 key layout type이 없다.
- 같은 Redis와 분리 Redis 구성에서 Location Store와 Relocation Store를 각각 등록해 사용할 수 있다.
