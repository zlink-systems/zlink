# Node.js Location·Relocation provider exact interface

[Node.js 공개 interface 목차](README.ko.md) ·
[Location Runtime](../../../../21-location-runtime.ko.md) ·
[Redis Location Store](../../../../22-location-store-redis.ko.md)

이 문서는 외부 provider가 구현하는 최소 public SPI와 공식 Redis extension의 public declaration을
고정한다. Authority, owner lease, reservation, capacity, aggregate와 relocation state machine은
Framework가 opaque record로 encoding한다. Provider는 해당 domain type이나 처리 단계를 알지 않는다.

Store primitive와 interface는 opt-in package
`@zlink-systems/framework-provider-abstractions`가 소유한다. Provider 구현은 이 package만 의존해 Store를
구현할 수 있으며 Actor·Spot application API에 의존하지 않는다.

## 1. Location Store

```ts
declare const zlinkStoreKeyBrand: unique symbol;
declare const zlinkStoreVersionBrand: unique symbol;
declare const zlinkStoreScanCursorBrand: unique symbol;

export interface ZLinkStoreKey {
  readonly value: string;
  readonly [zlinkStoreKeyBrand]: true;
}

export interface ZLinkStoreVersion {
  readonly value: string;
  readonly [zlinkStoreVersionBrand]: true;
}

export interface ZLinkStoreScanCursor {
  readonly value: string;
  readonly [zlinkStoreScanCursorBrand]: true;
}

export interface ZLinkStoreValue {
  readonly bytes: Uint8Array;
  readonly version: ZLinkStoreVersion;
  readonly expiresAt?: Date;
  readonly storeNow: Date;
}

export type ZLinkStoreReadResult =
  | { readonly kind: 'missing'; readonly storeNow: Date }
  | { readonly kind: 'found'; readonly value: ZLinkStoreValue };

export type ZLinkStoreCondition =
  | { readonly kind: 'missing'; readonly key: ZLinkStoreKey }
  | {
      readonly kind: 'version';
      readonly key: ZLinkStoreKey;
      readonly expected: ZLinkStoreVersion;
    };

export type ZLinkStoreMutation =
  | {
      readonly kind: 'put';
      readonly key: ZLinkStoreKey;
      readonly bytes: Uint8Array;
      readonly retentionMs?: number;
    }
  | { readonly kind: 'delete'; readonly key: ZLinkStoreKey };

export interface ZLinkStoreWriteRequest {
  readonly conditions: readonly ZLinkStoreCondition[];
  readonly mutations: readonly ZLinkStoreMutation[];
}

export interface ZLinkStorePutVersion {
  readonly key: ZLinkStoreKey;
  readonly version: ZLinkStoreVersion;
}

export type ZLinkStoreWriteResult =
  | {
      readonly kind: 'applied';
      readonly putVersions: readonly ZLinkStorePutVersion[];
      readonly storeNow: Date;
    }
  | { readonly kind: 'conflict'; readonly storeNow: Date };

export interface ZLinkStoreScanRequest {
  readonly prefix: string;
  readonly cursor?: ZLinkStoreScanCursor;
  readonly limit: number;
}

export interface ZLinkStoreScanItem {
  readonly key: ZLinkStoreKey;
  readonly value: ZLinkStoreValue;
}

export interface ZLinkStoreScanPage {
  readonly items: readonly ZLinkStoreScanItem[];
  readonly nextCursor?: ZLinkStoreScanCursor;
  readonly storeNow: Date;
}

export type ZLinkStoreScanResult =
  | { readonly kind: 'page'; readonly value: ZLinkStoreScanPage }
  | { readonly kind: 'expired' };

export interface ZLinkLocationStore {
  read(
    key: ZLinkStoreKey,
    signal?: AbortSignal
  ): Promise<ZLinkStoreReadResult>;

  write(
    request: ZLinkStoreWriteRequest,
    signal?: AbortSignal
  ): Promise<ZLinkStoreWriteResult>;

  scan(
    request: ZLinkStoreScanRequest,
    signal?: AbortSignal
  ): Promise<ZLinkStoreScanResult>;

  // Framework가 Store 수명을 인수한 경우 dependent runtime을 먼저 종료한 뒤 한 번 호출한다.
  dispose?(): void | Promise<void>;
}
```

Key는 Framework가 발급하는 opaque UTF-8 `1..1024` bytes 문자열이며 case-sensitive exact match를
사용한다. Version과 cursor는 provider가 발급하는 opaque UTF-8 `1..4096` bytes 문자열이다.
Value는 최대 1 MiB다. `retentionMs`가 없으면 만료되지 않으며, 만료 판단에는 provider clock을
사용한다. `storeNow`는 같은 provider 관측에서 얻은 시각이므로 Framework는 local clock을 TTL
판정에 사용하지 않는다. 지정한 `retentionMs`는 양의 safe integer여야 한다.

Framework는 provider Promise가 settle될 때까지 전달한 `Uint8Array`를 변경하거나 다른 operation의
buffer로 재사용하지 않는다. Provider는 반환한 `Uint8Array`를 Promise가 settle된 뒤 변경하거나 다른
결과와 공유하지 않는다.

`write(...)`는 모든 condition을 먼저 검사하고 모두 참일 때만 모든 mutation을 하나의 atomic
commit으로 적용한다. 조건 하나라도 거짓이면 mutation과 version 증가는 모두 0이고 `conflict`를
반환한다. Condition은 Missing 또는 exact Version 비교만 제공한다. Conflict 결과에 domain state나
current value를 싣지 않으며 Framework가 필요한 key를 exact read한다.

한 write request는 condition과 mutation을 합쳐 최대 2,048개의 unique key와 최대 4 MiB의 encoded
크기를 허용한다. 같은 key의 condition 또는 mutation을 중복할 수 없다.

`scan(...)`은 recovery와 maintenance가 bounded key set을 찾기 위한 필수 operation이다. Prefix는
UTF-8 `0..1024` bytes이고 limit은 `1..1000`이다. 첫 page가 만든 snapshot은 마지막 page까지
고정된다. Provider가 snapshot을 더 유지할 수 없으면 `expired`를 반환하고 Framework는 부분 결과를
버린 뒤 첫 page부터 다시 읽는다. 한 page는 encoded 4 MiB에 도달하면 limit보다 적은 item을 반환할
수 있다.

## 2. Relocation Store

```ts
declare const zlinkBlobReferenceBrand: unique symbol;

export interface ZLinkBlobReference {
  readonly value: string;
  readonly [zlinkBlobReferenceBrand]: true;
}

export type ZLinkBlobPutResult =
  | {
      readonly kind: 'stored' | 'alreadyStored';
      readonly expiresAt: Date;
      readonly storeNow: Date;
    }
  | { readonly kind: 'conflict'; readonly storeNow: Date };

export type ZLinkBlobReadResult =
  | { readonly kind: 'missing'; readonly storeNow: Date }
  | {
      readonly kind: 'found';
      readonly bytes: Uint8Array;
      readonly expiresAt: Date;
      readonly storeNow: Date;
    };

export type ZLinkBlobRenewResult =
  | { readonly kind: 'missing'; readonly storeNow: Date }
  | {
      readonly kind: 'renewed';
      readonly expiresAt: Date;
      readonly storeNow: Date;
    };

export interface ZLinkRelocationStore {
  put(
    reference: ZLinkBlobReference,
    payload: Uint8Array,
    retentionMs: number,
    signal?: AbortSignal
  ): Promise<ZLinkBlobPutResult>;

  read(
    reference: ZLinkBlobReference,
    signal?: AbortSignal
  ): Promise<ZLinkBlobReadResult>;

  renew(
    reference: ZLinkBlobReference,
    retentionMs: number,
    signal?: AbortSignal
  ): Promise<ZLinkBlobRenewResult>;

  // Reference가 없어도 성공하는 idempotent operation이다.
  delete(
    reference: ZLinkBlobReference,
    signal?: AbortSignal
  ): Promise<void>;

  dispose?(): void | Promise<void>;
}
```

Reference는 Framework가 put 전에 발급하는 opaque UTF-8 `1..4096` bytes 문자열이며 exact match를
사용한다. 삭제되거나 만료된 reference도 다른 content에 다시 사용하지 않는다. 같은 reference와 같은
bytes를 다시 put하면 `alreadyStored`, 다른 bytes를 put하면 `conflict`다. 이 규칙으로 Framework는
timeout이나 연결 오류 뒤에 같은 reference를 exact read하여 저장 결과를 재조정할 수 있다.
`retentionMs`는 양의 safe integer여야 한다.

Blob 하나는 최대 64 MiB다. Framework는 최대 4,096개의 chunk와 immutable root manifest를 사용해
최대 256 GiB의 logical relocation stream을 구성한다. Checksum, root·chunk 관계, participant
inventory와 relocation phase는 Framework가 소유하며 provider는 payload를 해석하지 않는다.

## 3. 등록과 수명

Application은 기존 `addLocationStore(...)`와 `addRelocationStore(...)`로 두 capability를 각각 한 번
등록한다. 같은 의미의 `use*` method나 Redis 전용 등록 helper는 제공하지 않는다. 등록이 성공하면
Store instance의 수명은 Framework가 인수한다. Framework는 dependent runtime과 진행 중인 operation을
먼저 종료한 뒤 Store의 `dispose()`가 있으면 정확히 한 번 호출한다. 두 Store가 connection을 공유하면
각 Store의 dispose 뒤 connection을 닫을지는 provider가 reference count나 외부 ownership으로 관리한다.

호출 전에 `AbortSignal`이 중단되면 provider는 I/O와 commit을 시작하지 않는다. 호출을 시작한 뒤
중단되거나 transport 오류가 발생하면 commit 여부가 불확실할 수 있다. Framework는 Location Store의
exact read와 version 또는 Relocation Store의 Framework-issued reference로 결과를 재조정한다.

## 4. Redis extension

```ts
export interface ZLinkRedisLocationOptions {
  readonly url?: string;
  readonly client?: RedisClientType;
  readonly clientOptions?: RedisClientOptions;
  readonly keyPrefix: string;
  readonly operationTimeoutMs?: number;
}

export interface ZLinkRedisRelocationOptions {
  readonly url?: string;
  readonly client?: RedisClientType;
  readonly clientOptions?: RedisClientOptions;
  readonly keyPrefix: string;
  readonly operationTimeoutMs?: number;
}

export class ZLinkRedisLocationStore implements ZLinkLocationStore {
  constructor(options: ZLinkRedisLocationOptions);
  dispose(): Promise<void>;
}

export class ZLinkRedisRelocationStore implements ZLinkRelocationStore {
  constructor(options: ZLinkRedisRelocationOptions);
  dispose(): Promise<void>;
}
```

공식 Redis package가 공개하는 provider 표면은 options 두 개와 Store 구현 class 두 개다. Redis key
layout, Lua script, private record encoding, retry와 connection lease는 implementation detail이다. 두
Store는 같은 Redis deployment와 client를 공유하거나 물리적으로 분리할 수 있다. 같은 deployment를
사용해도 서로 다른 `keyPrefix`를 사용하며 cross-store transaction은 요구하지 않는다.

## 5. 공개하지 않는 타입

다음 타입과 operation은 Framework private record 또는 Redis implementation detail이다.

- Authority·owner lease·reservation·capacity·fence·aggregate DTO
- `reserve`, `commit`, `abort`, `prepareAggregate` 같은 domain operation
- relocation phase·manifest·participant DTO와 provider-generated relocation reference
- raw Redis command adapter, script와 key codec
- Spot·Actor 전용 Store와 capability별 Store interface

Application이 사용하는 location 운영 query는
[Location, monitoring과 metrics](03-location-observability.ko.md)가 단독으로 소유한다.
