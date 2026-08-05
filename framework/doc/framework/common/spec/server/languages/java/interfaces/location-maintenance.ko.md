# Java Location·Relocation 공개 인터페이스

[Java exact interface 목록](README.ko.md) · [공통 Location runtime](../../../../21-location-runtime.ko.md) ·
[공통 Redis provider](../../../../22-location-store-redis.ko.md)

이 문서는 application과 provider plugin 작성자가 알아야 하는 Java public contract만 정의한다.
Authority, owner lease, reservation, capacity, aggregate와 relocation state machine은 Framework가 private
record로 구성한다. Provider는 record의 의미를 해석하지 않고 opaque key·value와 immutable blob만 저장한다.

Store primitive와 interface는 opt-in artifact
`systems.zlink:zlink-framework-provider-abstractions`의
`systems.zlink.framework.locationprovider` package가 소유한다. Provider 구현은 이 artifact만으로 Store
계약을 구현할 수 있으며 Actor·Spot application package에 의존하지 않는다.

## Provider 등록과 수명

Application은 기존 `ZLinkFrameworkOptions.addLocationStore(...)`와
`addRelocationStore(...)`로 두 Store를 각각 등록한다. 두 Store를 묶거나 Redis를 직접 등록하는 별도
helper는 제공하지 않는다.

등록이 성공하면 Store instance의 수명은 Framework로 이전된다. Store가 `AutoCloseable`을 구현하면
Framework는 dependent runtime을 먼저 종료한 뒤 정확히 한 번 닫는다. 두 Store가 connection을 공유할
때 각 Store가 해제할 connection lease는 provider가 관리한다.

## Location Store

```java
public record ZLinkStoreKey(String value) {}
public record ZLinkStoreVersion(String value) {}
public record ZLinkStoreScanCursor(String value) {}

public record ZLinkStoreValue(
    byte[] bytes,
    ZLinkStoreVersion version,
    Instant expiresAt,
    Instant storeNow) {}

public sealed interface ZLinkStoreReadResult
    permits ZLinkStoreReadMissing, ZLinkStoreReadFound {}

public record ZLinkStoreReadMissing(Instant storeNow)
    implements ZLinkStoreReadResult {}

public record ZLinkStoreReadFound(ZLinkStoreValue value)
    implements ZLinkStoreReadResult {}

public sealed interface ZLinkStoreCondition
    permits ZLinkStoreMissingCondition, ZLinkStoreVersionCondition {}

public record ZLinkStoreMissingCondition(ZLinkStoreKey key)
    implements ZLinkStoreCondition {}

public record ZLinkStoreVersionCondition(
    ZLinkStoreKey key,
    ZLinkStoreVersion expected)
    implements ZLinkStoreCondition {}

public sealed interface ZLinkStoreMutation
    permits ZLinkStorePut, ZLinkStoreDelete {}

public record ZLinkStorePut(
    ZLinkStoreKey key,
    byte[] bytes,
    Duration retention)
    implements ZLinkStoreMutation {}

public record ZLinkStoreDelete(ZLinkStoreKey key)
    implements ZLinkStoreMutation {}

public record ZLinkStoreWriteRequest(
    List<ZLinkStoreCondition> conditions,
    List<ZLinkStoreMutation> mutations) {}

public sealed interface ZLinkStoreWriteResult
    permits ZLinkStoreWriteApplied, ZLinkStoreWriteConflict {}

public record ZLinkStoreWriteApplied(
    Map<ZLinkStoreKey, ZLinkStoreVersion> putVersions,
    Instant storeNow)
    implements ZLinkStoreWriteResult {}

public record ZLinkStoreWriteConflict(Instant storeNow)
    implements ZLinkStoreWriteResult {}

public record ZLinkStoreScanRequest(
    String prefix,
    ZLinkStoreScanCursor cursor,
    int limit) {}

public record ZLinkStoreScanItem(
    ZLinkStoreKey key,
    ZLinkStoreValue value) {}

public record ZLinkStoreScanPage(
    List<ZLinkStoreScanItem> items,
    ZLinkStoreScanCursor nextCursor,
    Instant storeNow) {}

public sealed interface ZLinkStoreScanResult
    permits ZLinkStoreScanPageResult, ZLinkStoreScanExpired {}

public record ZLinkStoreScanPageResult(ZLinkStoreScanPage value)
    implements ZLinkStoreScanResult {}

public record ZLinkStoreScanExpired()
    implements ZLinkStoreScanResult {}

public interface ZLinkLocationStore {
    CompletionStage<ZLinkStoreReadResult> read(
        ZLinkStoreKey key,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkStoreWriteResult> write(
        ZLinkStoreWriteRequest request,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkStoreScanResult> scan(
        ZLinkStoreScanRequest request,
        ZLinkStoreCancellation cancellation);
}
```

`ZLinkStorePut.retention == null`이면 만료되지 않는 durable value다.
`ZLinkStoreScanRequest.cursor == null`은 첫 page이고
`ZLinkStoreScanPage.nextCursor == null`은 마지막 page다. 반환한 `byte[]`는 caller가 결과를 사용하는
동안 provider가 변경하거나 다른 결과 buffer로 재사용하지 않는다.

### 값과 시간

- Key는 Framework가 발급하는 opaque UTF-8 `1..1024` bytes이며 case-sensitive exact match다.
- Version은 provider가 발급하는 opaque UTF-8 `1..4096` bytes다. Framework와 provider는 내부 구조나
  수치 크기를 해석하지 않는다.
- Value는 최대 1 MiB다. 만료 시각은 provider clock을 기준으로 판단한다.
- `storeNow`는 같은 read, commit 또는 scan page에서 얻은 provider clock 값이다. Framework local clock은
  TTL correctness에 사용하지 않는다.

### Atomic write

`write(...)`는 모든 condition을 먼저 검사하고 모두 참일 때만 모든 mutation을 하나의 commit으로
적용한다. 하나라도 거짓이면 `ZLinkStoreWriteConflict`이며 mutation과 version 증가는 0이다. 다른
caller는 commit의 중간 상태를 관찰할 수 없다.

- Missing condition은 key가 없거나 만료되었을 때만 참이다.
- Version condition은 현재 version이 exact match일 때만 참이다.
- Condition과 mutation의 unique key 합계는 최대 2,048개다.
- Request의 encoded 크기는 최대 4 MiB다.
- 같은 key의 중복 condition 또는 중복 mutation은 허용하지 않는다.
- Applied result는 각 put에 provider가 발급한 새 version을 반환한다.
- Conflict result는 실패한 condition이나 current value를 공개하지 않는다. Framework가 exact read로
  필요한 key를 다시 확인한다.

### Snapshot scan

`scan(...)`은 recovery와 maintenance가 bounded key set을 찾는 필수 operation이다.

- Prefix는 UTF-8 `0..1024` bytes이며 key와 같은 exact comparison을 사용한다.
- 첫 page가 고정한 snapshot을 다음 cursor page도 사용한다.
- Limit은 `1..1000`이다. Encoded page가 4 MiB에 먼저 도달하면 item을 더 적게 반환할 수 있다.
- Cursor는 opaque UTF-8 `1..4096` bytes다.
- Provider가 snapshot을 더 유지할 수 없으면 `ZLinkStoreScanExpired`를 반환한다. Framework는 부분
  결과를 버리고 첫 page부터 다시 읽는다.

## Relocation Store

```java
public record ZLinkBlobReference(String value) {}

public sealed interface ZLinkBlobPutResult
    permits ZLinkBlobStored, ZLinkBlobAlreadyStored, ZLinkBlobConflict {}

public record ZLinkBlobStored(
    Instant expiresAt,
    Instant storeNow)
    implements ZLinkBlobPutResult {}

public record ZLinkBlobAlreadyStored(
    Instant expiresAt,
    Instant storeNow)
    implements ZLinkBlobPutResult {}

public record ZLinkBlobConflict(Instant storeNow)
    implements ZLinkBlobPutResult {}

public sealed interface ZLinkBlobReadResult
    permits ZLinkBlobMissing, ZLinkBlobFound {}

public record ZLinkBlobMissing(Instant storeNow)
    implements ZLinkBlobReadResult {}

public record ZLinkBlobFound(
    byte[] bytes,
    Instant expiresAt,
    Instant storeNow)
    implements ZLinkBlobReadResult {}

public sealed interface ZLinkBlobRenewResult
    permits ZLinkBlobRenewMissing, ZLinkBlobRenewed {}

public record ZLinkBlobRenewMissing(Instant storeNow)
    implements ZLinkBlobRenewResult {}

public record ZLinkBlobRenewed(
    Instant expiresAt,
    Instant storeNow)
    implements ZLinkBlobRenewResult {}

public interface ZLinkRelocationStore {
    CompletionStage<ZLinkBlobPutResult> put(
        ZLinkBlobReference reference,
        byte[] payload,
        Duration retention,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkBlobReadResult> read(
        ZLinkBlobReference reference,
        ZLinkStoreCancellation cancellation);

    CompletionStage<ZLinkBlobRenewResult> renew(
        ZLinkBlobReference reference,
        Duration retention,
        ZLinkStoreCancellation cancellation);

    CompletionStage<Void> delete(
        ZLinkBlobReference reference,
        ZLinkStoreCancellation cancellation);
}
```

Reference는 Framework가 put 전에 발급하는 opaque UTF-8 `1..4096` bytes이며 case-sensitive exact
match다. 같은 reference와 같은 bytes를 다시 put하면 `ZLinkBlobAlreadyStored`, 다른 bytes면
`ZLinkBlobConflict`다. 삭제되거나 만료된 reference도 다른 content에 재사용하지 않는다.

Blob 하나는 최대 64 MiB다. Framework는 최대 256 GiB logical relocation stream을 최대 4,096개의
64 MiB chunk와 immutable root manifest로 나눈다. Checksum과 root·chunk 관계는 Framework가 계산하고
검증하며 provider는 manifest를 해석하지 않는다.

Read는 원래 bytes와 provider clock 기준 expiry를 반환한다. Renew와 delete retry는 idempotent하며,
delete는 reference가 없어도 성공한 no-op이다. Framework가 reference를 미리 발급하므로 timeout이나
결과 유실 뒤 같은 reference를 exact read하여 저장 여부를 재조정할 수 있다.

## 취소와 오류

`ZLinkStoreCancellation`은 provider I/O operation의 cancellation만 표현한다. 호출 전에 cancellation이
요청되면 provider는 I/O나 commit을 시작하지 않는다. 호출이 시작된 뒤 cancellation, timeout 또는
transport 오류가 발생하면 commit 여부가 불확실할 수 있으며 Framework가 exact read와 version 또는
caller-issued blob reference로 결과를 재조정한다.

입력 범위 위반과 동일 key 중복은 `IllegalArgumentException`이다. Conflict, Missing, Expired와
AlreadyStored는 닫힌 정상 결과다. 그 밖의 Store 호출 예외는 Framework가 provider failure로 분류한다.

## 운영 조회

```java
public interface ZLinkLocationRuntimeQuery {
    CompletionStage<ZLinkLocationRuntimeStatus> getStatus();
    CompletionStage<ZLinkLocationPage<ZLinkLocationTopologyEntry>> listTopology(
        ZLinkLocationTopologyFilter filter,
        ZLinkPageRequest page);
    CompletionStage<ZLinkLocationPage<ZLinkLocationServiceSummary>> listServiceSummaries(
        ZLinkLocationServiceSummaryFilter filter,
        ZLinkPageRequest page);
}

public interface ZLinkLocationReadiness {
    CompletionStage<Boolean> isPeerReady(
        String meshName,
        ZLinkLocationRole role,
        RoutingId nodeRid);
}
```

운영 조회는 bounded page만 반환한다. Raw Spot·Actor authority row, Store key, scan cursor와 provider
version은 application 조회 계약에 포함하지 않는다.

## Redis extension

```java
public final class ZLinkRedisLocationStore
    implements ZLinkLocationStore, AutoCloseable {
    public ZLinkRedisLocationStore(ZLinkRedisLocationOptions options);
}

public final class ZLinkRedisRelocationStore
    implements ZLinkRelocationStore, AutoCloseable {
    public ZLinkRedisRelocationStore(ZLinkRedisRelocationOptions options);
}

public final class ZLinkRedisLocationOptions {
    public String connectionString();
    public ZLinkRedisLocationOptions setConnectionString(String value);
    public String keyPrefix();
    public ZLinkRedisLocationOptions setKeyPrefix(String value);
    public Duration operationTimeout();
    public ZLinkRedisLocationOptions setOperationTimeout(Duration value);
}

public final class ZLinkRedisRelocationOptions {
    public String connectionString();
    public ZLinkRedisRelocationOptions setConnectionString(String value);
    public String keyPrefix();
    public ZLinkRedisRelocationOptions setKeyPrefix(String value);
    public Duration operationTimeout();
    public ZLinkRedisRelocationOptions setOperationTimeout(Duration value);
}
```

Redis public surface는 두 Store class의 최소 constructor와 connection, key namespace, operation timeout
options로 제한한다. Key layout, Lua script, private record encoding, retry와 shared connection reference
count는 implementation detail이다.

## 공개하지 않는 계약

다음 항목은 provider나 application이 구현하거나 호출하는 interface가 아니다.

- Authority, owner lease, reservation, capacity와 aggregate별 Store capability
- Runtime publisher, resolver, cache, retry coordinator와 recovery state machine
- Redis script client, key codec, row serializer와 connection lease
- Watch publisher, change-stamp event와 raw peer·Spot·Actor·route Store
- Routing-ID slot, allocation group과 allocated-RID provider

Provider public declaration에는 Authority, Reservation, Aggregate, Capacity, Fence와 relocation phase
타입이 나타나지 않아야 한다.
