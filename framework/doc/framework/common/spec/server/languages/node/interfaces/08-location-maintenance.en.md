# Node.js Location/Relocation Provider Exact Interface

[Node.js public interface table of contents](README.en.md) ·
[Location Runtime](../../../../21-location-runtime.en.md) ·
[Redis Location Store](../../../../22-location-store-redis.en.md)

This document fixes the minimal public SPI an external provider
implements and the public declaration of the official Redis extension.
Authority, owner lease, reservation, capacity, aggregate, and the
relocation state machine are encoded by the framework as opaque
records. The provider doesn't know that domain type or processing
stage.

The Store primitives and interfaces are owned by the opt-in package
`@zlink-systems/framework-provider-abstractions`. A provider
implementation can implement the Store depending only on this package,
without depending on the Actor/Spot application API.

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

  // If the framework took over the Store's lifetime, it ends the dependent runtime first and then calls this once.
  dispose?(): void | Promise<void>;
}
```

Key is an opaque UTF-8 `1..1024`-byte string the framework issues,
compared with case-sensitive exact match. Version and cursor are
opaque UTF-8 `1..4096`-byte strings the provider issues. Value is at
most 1 MiB. If `retentionMs` is absent, it doesn't expire, and the
provider clock is used to judge expiry. Since `storeNow` is a time
obtained from the same provider observation, the framework doesn't use
the local clock for TTL judgment. The specified `retentionMs` must be a
positive safe integer.

The framework doesn't change the `Uint8Array` it passed, or reuse it as
a different operation's buffer, until the provider's Promise settles.
The provider doesn't change the returned `Uint8Array`, or share it with
a different result, after the Promise settles.

`write(...)` first checks every condition, and only if all are true
does it apply every mutation as one atomic commit. If even one condition
is false, both mutation and version increase are 0, and it returns
`conflict`. Condition only provides Missing or exact Version
comparison. The conflict result doesn't carry domain state or the
current value — the framework does an exact read of the needed key.

One write request allows at most 2,048 unique keys combining conditions
and mutations, and at most 4 MiB of encoded size. A condition or
mutation on the same key can't be duplicated.

`scan(...)` is the required operation recovery and maintenance use to
find a bounded key set. Prefix is UTF-8 `0..1024` bytes, and limit is
`1..1000`. The snapshot the first page created is fixed through the
last page. If the provider can't keep the snapshot any longer, it
returns `expired`, and the framework discards the partial result and
reads from the first page again. A page can return fewer items than
limit once it reaches 4 MiB encoded.

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

  // an idempotent operation that succeeds even when the reference doesn't exist.
  delete(
    reference: ZLinkBlobReference,
    signal?: AbortSignal
  ): Promise<void>;

  dispose?(): void | Promise<void>;
}
```

Reference is an opaque UTF-8 `1..4096`-byte string the framework issues
before put, using exact match. A deleted or expired reference also
isn't reused for different content. Re-putting the same reference with
the same bytes returns `alreadyStored`; putting different bytes returns
`conflict`. With this rule, the framework can reconcile the storage
result after a timeout or connection error by doing an exact read of
the same reference. `retentionMs` must be a positive safe integer.

One blob is at most 64 MiB. The framework composes a logical relocation
stream of at most 256 GiB using at most 4,096 chunks and an immutable
root manifest. Checksum, root/chunk relationship, participant inventory,
and relocation phase are owned by the framework, and the provider
doesn't interpret the payload.

## 3. Registration And Lifetime

The application registers each of the two capabilities once with the
existing `addLocationStore(...)` and `addRelocationStore(...)`. A
`use*` method of the same meaning, or a Redis-specific registration
helper, isn't provided. Once registration succeeds, the framework takes
over the Store instance's lifetime. The framework ends the dependent
runtime and in-progress operations first, and then calls the Store's
`dispose()`, if present, exactly once. If two Stores share a
connection, whether to close the connection after each Store's dispose
is managed by the provider with a reference count or external
ownership.

If `AbortSignal` is aborted before the call, the provider doesn't start
I/O or commit. If it's aborted or a transport error occurs after the
call has started, whether the commit was applied may be uncertain. The
framework reconciles the result with the Location Store's exact read
and version, or the Relocation Store's framework-issued reference.

## 4. Redis Extension

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

The provider surface the official Redis package makes public is two
options and two Store implementation classes. Redis key layout, Lua
script, private record encoding, retry, and connection lease are
implementation details. The two Stores can share the same Redis
deployment and client, or be physically separate. Even when using the
same deployment, they use different `keyPrefix`es, and a cross-store
transaction isn't required.

## 5. Types Not Made Public

The following types and operations are Framework-private records or
Redis implementation details.

- Authority/owner-lease/reservation/capacity/fence/aggregate DTOs
- Domain operations such as `reserve`, `commit`, `abort`,
  `prepareAggregate`
- Relocation phase/manifest/participant DTOs and a provider-generated
  relocation reference
- Raw Redis command adapter, script, and key codec
- Spot/Actor-dedicated Store and per-capability Store interfaces

The location operational query the application uses is owned solely by
[Location, Monitoring, And Metrics](03-location-observability.en.md).
