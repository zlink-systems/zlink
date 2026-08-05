---
title: "Relocation Store provider SPI와 공식 Redis 구현"
---

# Relocation Store provider SPI와 공식 Redis 구현

[스펙 목차](README.ko.md) · [이전: Location Store provider SPI와 공식 Redis 구현](22-location-store-redis.ko.md) · [다음: Runtime 상태 조회와 운영 진단](24-runtime-monitoring.ko.md)

> **이 장이 정의하는 것** — relocation과 복구에 필요한 byte payload를 보관하는
> Relocation Store의 공개 provider interface(SPI).


## 1. 이 문서가 정하는 계약

이 문서는 Framework의 relocation과 복구에 필요한 byte payload를 보관하는 Relocation Store의
공개 provider interface를 정의한다. 이처럼 Framework가 호출하고 외부 provider가 구현하는
interface를 SPI라고 한다. Provider 개발자는 Framework가 만든 reference를 그대로 key로 사용하여
payload를 저장하고, 같은 reference로 읽기·보존 기간 연장·삭제를 수행해야 한다.

Application은 이 SPI를 직접 호출하지 않는다. Provider package가 SPI를 구현하고, Framework가 등록된
provider instance를 사용한다.

주소와 상태를 가진 논리 instance인 [Spot](01-glossary.ko.md#spot)의 현재 실행 책임 node를
[owner](01-glossary.ko.md#owner)라고 한다. 이 owner와 lifecycle 상태를 보관하고 생성 권한을 조정하는
저장소가 [Location Store](01-glossary.ko.md#location-store)다. Relocation Store는 이 authority를 관리하지
않고, Location Store에 게시하기 전의 payload와 이미 게시된 payload만 reference로 보관한다. 두 Store를
연결하여 relocation과 복구를 진행하는 순서는 [Location runtime](21-location-runtime.ko.md)이 정한다.

Provider는 저장할 bytes의 업무 의미를 해석하지 않는다. Application state, 처리 수락 기록, timer,
이동 대상과 payload reference 목록, relocation 단계뿐 아니라 최초 application message와 생성
정보를 묶은 [activation envelope](01-glossary.ko.md#activation-envelope)도 해석하지 않은 bytes로
저장한다. 최초
message를 계기로 필요한 시점에 만들 수 있는 [Instance Spot](01-glossary.ko.md#entry-spot-user-spot과-instance-spot)이
아직 실행 중이 아닐 때 새 instance를 만들고 그 message를 처리할 수 있게 준비하는
[cold activation](01-glossary.ko.md#cold-activation)과 Actor Join의 실행 절차도 이 문서의 범위가 아니다.

## 2. 공개 SPI와 책임 경계

Relocation Store SPI가 제공하는 operation과 provider의 책임은 다음 네 가지뿐이다.

| Operation | Provider가 보장하는 결과 |
|---|---|
| `Put` | Framework가 발급한 reference에 immutable payload를 저장하거나, 이미 저장된 payload와 byte 단위로 같은지 확인한다. |
| `Read` | 지정한 reference의 immutable payload, 만료 시점과 provider 기준 현재 시각을 함께 반환한다. |
| `Renew` | Provider clock을 기준으로 보존 기간을 다시 계산한다. |
| `Delete` | 지정한 reference를 제거한다. Reference가 없어도 성공한다. |

SPI type과 interface는 기본 Framework API와 분리된 provider abstraction package 또는 module이
소유한다. 기본 Framework package는 이 abstraction에 의존하지만 Store operation을 application API로
노출하지 않는다. 외부 provider는 application·Actor·Spot package에 의존하지 않고 abstraction만
구현할 수 있어야 한다.

Relocation 단계, manifest, participant, replay cursor와 completion마다 별도 public method나 DTO를
추가하지 않는다. Redis key 배치, chunk 저장 구조, script와 cleanup index도 공개 SPI에 노출하지 않는다.

다음 .NET 발췌는 공통 SPI의 최소 형태를 보여준다. 정식 선언은
[.NET exact interface](server/languages/dotnet/interfaces/08-authority-relocation.ko.md)에 있다.

```csharp
public interface IZLinkRelocationStore
{
    // Framework가 만든 reference를 바꾸지 않고 payload를 저장한다.
    ValueTask<ZLinkBlobPutResult> PutAsync(
        ZLinkBlobReference reference,
        ReadOnlyMemory<byte> payload,
        TimeSpan retention,
        CancellationToken cancellationToken = default);

    // 같은 reference의 payload와 provider 기준 만료 정보를 함께 읽는다.
    ValueTask<ZLinkBlobReadResult> ReadAsync(
        ZLinkBlobReference reference,
        CancellationToken cancellationToken = default);

    // Payload를 변경하지 않고 provider clock을 기준으로 만료 시점만 연장한다.
    ValueTask<ZLinkBlobRenewResult> RenewAsync(
        ZLinkBlobReference reference,
        TimeSpan retention,
        CancellationToken cancellationToken = default);

    // Reference가 없어도 성공하므로 안전하게 다시 호출할 수 있다.
    ValueTask DeleteAsync(
        ZLinkBlobReference reference,
        CancellationToken cancellationToken = default);
}
```

다른 언어의 정식 선언은
[Java](server/languages/java/interfaces/location-maintenance.ko.md),
[Kotlin](server/languages/kotlin/interfaces/location-maintenance.ko.md),
[Node.js](server/languages/node/interfaces/08-location-maintenance.ko.md)와
[C++](server/languages/cpp/interfaces/07-location-store.ko.md) exact interface를 따른다.
두 Store의 등록 예제는 [Location runtime](21-location-runtime.ko.md#13-등록-조건과-수명)에만 둔다.

## 3. Reference와 저장 크기

Provider가 해석해야 하는 값은 reference, payload, retention뿐이다.

| 항목 | 계약 |
|---|---|
| Reference | Framework가 `Put` 전에 발급하는 opaque UTF-8 `1..4096` bytes다. 대소문자를 구분하여 전체 값이 정확히 같은지 비교한다. |
| Application data chunk | Framework가 나누기 전 application bytes 기준으로 최대 64 MiB다. |
| Redis encoded blob | Data chunk와 Framework가 붙이는 immutable envelope를 합친 provider 입력이다. 공식 Redis provider의 최대 크기는 `64 MiB + 23 bytes`다. |
| 여러 blob으로 나눈 payload | Framework가 여러 blob으로 구성할 수 있는 전체 payload다. 최대 크기는 256 GiB다. |
| Chunk 수 | 전체 payload의 맨 앞 목록이 가리키는 data chunk의 합계는 최대 4,096개다. |
| Ordered stripe 수 | 하나의 relocation root를 병렬로 저장하고 읽기 위해 나누는 연속 구간은 최대 64개다. |
| `StoreNow` | Provider가 `Put`·`Read`·`Renew` 결과에 넣는 현재 시각이다. 만료 여부는 이 시각과 provider clock으로 판단한다. |

Provider는 reference를 만들거나 바꾸지 않는다. 같은 content라도 Framework가 서로 다른 reference를
지정하면 별개의 value로 저장한다. 삭제되거나 만료된 reference를 다른 bytes에 다시 사용해서는 안 된다.

Framework는 64 MiB보다 큰 payload를 application bytes 기준 최대 64 MiB인 data chunk로
나눈다. 각 chunk에는 checksum과 복구에 필요한 23-byte immutable envelope를 붙인 뒤
Redis provider에 전달한다. 따라서 application data 제한과 provider가 받는 encoded blob
제한은 23 bytes만큼 다르다. 별도의 맨 앞
목록에는 format version, 전체 길이, checksum, chunk 순서와 각 chunk의
reference·길이·checksum을 기록한다. Provider는 이 목록도 일반 bytes로 저장한다. 목록의
내용이나 chunk 관계는 Framework가 확인한다.

Framework는 participant 수와 payload 크기를 보고 relocation root 전체를 최대 64개의
연속 구간으로 균등하게 나눈다. 이 구간을 ordered stripe라고 한다. Stripe는 특정 Actor나
Spot의 payload를 뜻하지 않는다. Relocation Store는 stripe의 내용을 해석하지 않고 opaque
bytes로 저장한다.

Stripe가 64 MiB보다 크면 최대 64 MiB인 data chunk로 다시 나눈다. 모든 stripe가 가리키는
data chunk의 합계는 4,096개를 넘을 수 없다. SpotWide User Spot에 Actor가 100개 있어도
participant마다 blob 하나를 만들지 않고 최대 64개 stripe를 병렬로 처리한다. 한 process가
병렬 I/O 결과로 보관하는 encoded chunk bytes 합계는 기본 256 MiB를 넘지 않는다.

저장할 때는 모든 data chunk를 다시 읽어 bytes와 checksum을 확인한 뒤에만 맨 앞 목록을
저장한다. 일부 chunk의 저장이나 확인이 실패하면 맨 앞 목록을 저장하지 않는다. 이때 남은
chunk는 어떤 Location Store record도 가리키지 않으므로 retention 만료로 정리한다.

복원할 때는 data chunk를 병렬로 읽고 각각의 checksum을 확인한다. I/O 완료 순서와 관계없이
맨 앞 목록에 기록된 stripe와 chunk 순서로 합친다. 합친 bytes의 전체 checksum도 일치해야
Spot state와 Actor state를 복원한다. Stripe 하나라도 없거나 checksum이 다르면 전체
relocation unit을 `DataLost`로 처리한다. 일부 participant만 복원하지 않는다. 보관 기간을
연장할 때도 각 data chunk의 존재와 checksum을 병렬로 확인하고, 모두 성공한 뒤 맨 앞
목록의 보관 기간을 연장한다.

Application state adapter 하나가 반환할 수 있는 bytes도 최대 64 MiB다. Process 하나에서 relocation
payload를 동시에 처리할 때 적용하는 기본 256 MiB 제한은 Framework coordinator의 실행 중 memory
제한이다. 이 값은 blob 하나나 여러 blob으로 나눈 전체 payload의 저장 크기 제한을
바꾸지 않는다.

각 data chunk와 맨 앞 목록의 기본 retention은 24시간이고, Framework는 남은 retention이 12시간이 되는
시점을 기본 renew threshold로 사용한다. Provider는 자신의 clock으로 expiry를 계산해야 한다.
Application host의 wall clock을 만료 판단에 사용하지 않는다.

## 4. Operation별 결과

### 4.1 `Put`

`Put(reference, payload, retention)`은 다음 결과 중 하나만 반환한다.

- `Stored(expiresAt, storeNow)`: Reference가 없어서 payload를 새로 저장했다.
- `AlreadyStored(expiresAt, storeNow)`: 같은 reference에 같은 bytes가 이미 저장되어 있다.
- `Conflict(storeNow)`: 같은 reference에 다른 bytes가 저장되어 있다.

Provider는 payload 전체를 byte 단위로 비교한다. 같은 content에 새 reference를 발급하거나 provider가
선택한 reference를 반환하는 API를 제공하지 않는다.

### 4.2 `Read`

`Read(reference)`는 만료되지 않은 payload가 있으면
`Found(bytes, expiresAt, storeNow)`를 반환하고, reference가 없거나 만료되었으면
`Missing(storeNow)`을 반환한다. `Found`의 bytes는 consumer가 사용하는 동안 변경하거나 다른 read
결과의 buffer로 재사용해서는 안 된다.

### 4.3 `Renew`

`Renew(reference, retention)`은 provider clock을 기준으로 새 expiry를 계산한다. Payload가 있으면
`Renewed(expiresAt, storeNow)`를 반환하고, reference가 없거나 이미 만료되었으면
`Missing(storeNow)`을 반환한다. 같은 요청을 반복해도 payload bytes는 바뀌지 않는다.

### 4.4 `Delete`

`Delete(reference)`는 reference가 없을 때도 성공하는 idempotent operation이다. 같은 요청을 여러 번
실행해도 최종 결과는 reference가 없는 상태로 같다.

## 5. 취소, 오류와 결과 재구성

Operation을 시작하기 전에 cancellation이 요청되면 provider는 I/O와 write를 시작하지 않는다.
Operation을 시작한 뒤 cancellation, timeout 또는 transport error가 발생하면 저장이나 삭제가
적용되었는지 알 수 없을 수 있다. Provider는 이 경우를 성공이나 정상 결과로 추정하지 않는다.

공식 Redis provider의 `OperationTimeout`은 connection을 얻는 시간과 Redis command가 끝나는
시간을 합친 operation 전체에 적용한다. 제한 시간이 지나면 provider waiter는 언어별 timeout으로
완료되고 Framework public operation은 이를 `DeadlineExceeded`로 변환한다. 이미 Redis에 전달한 write는 timeout 뒤에도 적용될 수 있으므로 실패했다고
추정하지 않는다.

`Put` 결과를 받지 못한 Framework는 자신이 발급한 reference로 `Read`를 실행하거나, 같은 reference와
같은 bytes로 `Put`을 다시 실행하여 저장 여부를 재구성할 수 있어야 한다. `Delete`는 다시 실행해도
같은 상태가 되며, `Renew`도 payload를 변경하지 않는다.

Reference 길이, payload 크기와 retention 등 입력 계약을 위반하면 언어별 argument validation error를
반환한다. `Missing`, `AlreadyStored`와 `Conflict`는 provider 장애가 아니라 호출자가 처리할 수 있는
정상 결과다. 그 밖의 provider-specific failure는 Framework가 Store failure로 분류할 수 있어야 한다.
Redis command, key 배치와 script 정보는 application public API에 노출하지 않는다.

Caller가 넘긴 input bytes는 asynchronous operation이 끝날 때까지 변경되지 않아야 한다. Provider가
operation 완료 뒤에도 같은 memory를 참조하려면 먼저 bytes를 복사해야 한다.

## 6. Payload 게시와 정리

Location Store authority가 아직 가리키지 않는 payload를 orphan이라고 한다. Location Store 게시 전에
작업이 중단되면 provider 또는 Framework cleanup이 retention 만료 뒤 해당 orphan을 제거해야 한다.

Location Store authority가 가리키는 published reference는 아직 복구에 필요할 수 있다. Framework는
Location Store에서 해당 reference의 사용 종료를 먼저 commit한 뒤 payload를 삭제해야 한다. Provider는
retention이 남아 있는 published payload를 임의로 삭제하지 않는다. 이 게시·해제 순서와 payload가
없을 때의 `DataLost` 처리는
[Location runtime의 결과 재구성 규칙](21-location-runtime.ko.md#8-store-응답을-받지-못했을-때)이 정한다.

## 7. 등록과 provider instance 수명

Provider instance의 등록 조건과 Framework root의 소유권은
[Location runtime의 Store 등록](21-location-runtime.ko.md#13-등록-조건과-수명)을 따른다. Framework가
instance 수명을 소유하는 구성에서는 Store를 사용하는 runtime과 background operation을 모두 종료한
뒤 instance를 정확히 한 번 dispose한다.

여러 Store instance가 하나의 물리 connection을 공유할 수 있다. 각 instance를 dispose할 때 connection을
언제 해제할지 결정하고 중복 해제를 막는 책임은 provider 구현에 있다.

## 8. 공식 Redis provider

공식 Redis extension package는 언어별 naming convention에 맞는 `RedisRelocationStore` 구현을 제공한다.
공개 options는 instance 생성에 필요한 connection, key namespace와 operation timeout으로 제한한다.

다음 항목은 Redis provider 내부 구현이며 public contract가 아니다.

- Redis key 배치와 chunk 저장 자료구조
- Script와 private serialization record
- Connection lease와 cleanup index
- Retry와 cleanup을 실행하는 내부 방식

Redis 전용 Framework 등록 helper나 Location Store와 Relocation Store를 함께 구현하는 결합 class는
제공하지 않는다.

Location Store와 Relocation Store는 같은 Redis deployment에서 서로 다른 key namespace를 사용할 수도
있고, 서로 다른 deployment에 둘 수도 있다. 공개 계약의 correctness는 connection 공유나 두 Store를
묶은 Redis transaction에 의존하지 않는다.

## 9. Contract test 요구 사항

구현과 언어별 contract test는 다음 결과를 확인해야 한다.

- 같은 reference와 같은 bytes를 다시 `Put`하면 `AlreadyStored`, 다른 bytes를 저장하면 `Conflict`다.
- 64 MiB application data와 23-byte envelope로 구성한 encoded blob, 최대 4,096개
  data chunk와 256 GiB 전체 payload 계약을 지원한다.
- Participant 수와 관계없이 relocation root를 최대 64개의 ordered opaque stripe로
  균등하게 나누고, 원래 순서와 전체 checksum을 보존한다.
- `Put` 결과를 받지 못한 뒤 exact `Read`나 같은 입력의 `Put`으로 저장 여부를 재구성할 수 있다.
- Redis operation timeout은 connection 획득과 실제 command를 제한하며, timeout 뒤 완료된
  write는 같은 reference와 bytes를 사용한 retry로 `AlreadyStored`임을 확인할 수 있다.
- `Read`가 반환한 bytes는 consumer가 사용하는 동안 변경되지 않는다.
- `Renew`와 `Delete`를 다시 실행해도 payload가 달라지지 않으며, expiry는 provider clock으로 계산한다.
- Published reference의 사용 종료를 Location Store에 commit하기 전에 payload를 삭제하지 않는다.
- Location Store 게시 전에 실패한 payload는 retention 만료 뒤 orphan cleanup 대상이 된다.
- Location Store와 Relocation Store를 같은 Redis와 서로 다른 Redis 구성에 각각 등록할 수 있다.
- Redis provider의 public declaration에는 relocation 단계·manifest DTO, script와 key 배치 type이 없다.
