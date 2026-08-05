# 07. Location authority

[레퍼런스 목차](README.ko.md)

이 category는 Location·Relocation Store 등록, `ZLinkLocationOptions` 조정, `ZLinkLocationReadiness`와
`ZLinkLocationRuntimeQuery`(`ZLINK_LOCATION_RUNTIME_QUERY`)가 제공하는 진입점을 다룬다. 정확한
signature는
[Location·Relocation provider exact interface](../../common/spec/server/languages/node/interfaces/08-location-maintenance.ko.md),
[NestJS host adapter exact interface](../../common/spec/server/languages/node/interfaces/07-nestjs-host.ko.md)와
[Location 운영 조회와 observability exact interface](../../common/spec/server/languages/node/interfaces/03-location-observability.ko.md)가
소유한다.

---

## Location·Relocation Store 등록 (구성 시점)

분산 discovery, Instance Spot cold activation 또는 Actor·Spot relocation을 쓰는 host가 Store
구현을 root에 등록한다.

```ts
zlinkFramework()
  .addLocationStore(new ZLinkRedisLocationStore({
    url: "redis://redis-host:6379",
    keyPrefix: "zlink:game:location",
  }))
  .addRelocationStore(new ZLinkRedisRelocationStore({
    url: "redis://redis-host:6379",
    keyPrefix: "zlink:game:relocation",
  }));
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.addLocationStore(store: ZLinkLocationStore)` | 없으면 분산 discovery·relocation 불가 | exact read, conditional atomic write(`write`), bounded prefix scan(`scan`)을 제공하는 Store 하나 |
| `.addRelocationStore(store: ZLinkRelocationStore)` | `recreateOnRelocation()`/`preserveStateWith(...)` factory나 Instance Spot factory가 하나라도 있으면 필수 | Framework가 발급한 reference에 immutable relocation payload를 저장하는 Store 하나 |
| `ZLinkRedisLocationOptions.keyPrefix` / `ZLinkRedisRelocationOptions.keyPrefix` | 유효한 구성에는 반드시 비어 있지 않은 값을 지정해야 한다(둘이 같은 Redis를 쓰면 서로 달라야 함) | Redis key namespace |
| `.url` 또는 `.client`/`.clientOptions` | 필수(하나 이상) | Redis 연결 설정 |
| `.operationTimeoutMs` | 구현 기본값 | provider I/O 상한 |

**완료 결과.** 반환값 없이 동기로 등록된다. 각 역할은 정확히 하나만 등록한다 — 같은 역할을 두 번
등록하거나 필수 Store가 없으면 startup 검증에서 configuration error로 드러난다. Store에 `dispose()`가
있으면 Framework가 dependent runtime을 먼저 종료한 뒤 정확히 한 번 호출한다.

**선택 기준.** Manual peer만 쓰고 분산 location 기능이 필요 없는 node는 이 항목을 생략하고 시작할
수 있다. 공식 Redis provider 외에 같은 `ZLinkLocationStore`/`ZLinkRelocationStore`(opt-in package
`@zlink-systems/framework-provider-abstractions`만 의존)를 구현하는 다른 provider도 등록할 수
있다.

---

## `configureLocations()` (구성 시점)

Owner lease, polling과 relocation 동시성 상한을 조정한다.

```ts
zlinkFramework().configureLocations()
  .ownerLeaseTtlMs(20_000)
  .maxConcurrentRelocationCaptures(16);
```

**옵션.** 자주 조정하는 값은 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.ownerLeaseRenewIntervalMs(value)` / `.ownerLeaseTtlMs(value)` / `.ownerLeaseFencingMarginMs(value)` / `.ownerLeaseRenewTimeoutMs(value)` | 5000 / 15000 / 5000 / 3000 | Owner lease 갱신 주기와 유효기간(ms). `renewInterval + renewTimeout < ttl - fencingMargin`을 만족해야 한다 |
| `.pollingIntervalMs(value)` | 1000 | Store 상태 확인 주기 |
| `.storeFailureGraceMs(value)` | 30000 | Store 장애를 감내하는 유예 시간 |
| `.routeCacheMaxAgeMs(value)` / `.messageFollowDurationMs(value)` | 15000 / 30000 | `0`이면 기능을 끈다. 둘 다 양수면 cache age가 message follow duration보다 최소 5초 작아야 한다 |
| `.maxActiveOutboundRelocations(value)` / `.maxActiveInboundRelocations(value)` | 64 / 64 | 동시 진행 가능한 relocation unit 상한 |
| `.maxConcurrentRelocationCaptures(value)` / `.maxConcurrentRelocationRestores(value)` | 8 / 8 | 동시 실행 가능한 Capture·Restore callback 상한 |
| `.maxRelocationPayloadInFlightBytes(value)` | 268,435,456 | process 전체 encoded relocation payload in-flight 상한 |

**완료 결과.** 각 modifier는 `ZLinkLocationOptions`를 반환하는 동기 fluent 호출이다. Lease·polling
값이 0 이하이거나 위 부등식을 어기면 startup 검증에서 드러난다. 실행 중 값 변경은 새 relocation
admission에만 적용된다.

**선택 기준.** 기본값이 배포 환경(네트워크 지연, Store 응답 시간)에 맞지 않을 때만 조정한다.

---

## `isPeerReady` (ZLinkLocationReadiness)

특정 MeshName·role(선택적으로 특정 node)의 peer가 준비됐는지 확인한다.

```ts
const ready = await locationReadiness.isPeerReady("play", ZLinkLocationRole.Spot);
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `nodeRid` | 생략(role 전체 기준) | 특정 node로 좁혀서 확인 |
| `signal` | 없음 | 이 `Promise`의 대기만 취소한다 |

**완료 결과.** `Promise<boolean>`을 반환한다. 별도 실패 kind 없이 준비 여부만 알려준다.

**선택 기준.** 특정 역할의 peer가 준비될 때까지 기다리는 startup 순서 제어나 헬스체크에 쓴다.

---

## `getStatus` (ZLinkLocationRuntimeQuery)

Location runtime 자체의 상태(Store 연결, owner lease 갱신)를 확인한다.

```ts
const status = await locationQuery.getStatus();
```

**옵션.** 이 진입점에는 선택적 `signal`만 있다.

**완료 결과.** `ZLinkLocationRuntimeStatus`를 반환한다. Store 연결과 owner lease 갱신 상태를
나타내는 field로 구성된다.

**선택 기준.** Location 인프라 자체의 건강 상태를 진단할 때 쓴다. 특정 peer 준비 여부는
`isPeerReady`를 쓴다.

---

## `listTopology` / `listServiceSummaries` (ZLinkLocationRuntimeQuery)

등록된 node topology나 MeshName별 서비스 요약을 페이지 단위로 조회한다.

```ts
const page = await locationQuery.listTopology(
  { meshName: "play", state: ZLinkLocationTopologyState.Ready },
  { pageSize: 200 },
);
```

**옵션.** 두 호출 모두 다음 modifier를 받는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| filter(`ZLinkLocationTopologyFilter`/`ZLinkLocationServiceSummaryFilter`) | 전체(모든 field 생략) | MeshName·NodeRid·State로 결과를 좁힌다 |
| `page.pageSize` | 100 | 1..1000 범위의 정수 |
| `page.continuationToken` | 생략(첫 페이지) | 이전 응답이 반환한 opaque token. Application이 직접 해석하거나 다른 query에 재사용하지 않는다 |

**완료 결과.** `ZLinkLocationPage<T>`를 반환한다. Continuation token이 없으면 마지막 페이지다.
Store key·version, owner lease generation, descriptor payload 같은 내부 정보는 반환하지 않는다.

**선택 기준.** 운영 도구에서 등록된 node나 서비스 현황을 사람이 볼 수 있는 형태로 조회할 때 쓴다.
단일 MeshName·ChannelName의 실시간 가용성 판단에는 topology-discovery category의 상태 조회
항목을 쓴다.

---

전체 근거는
[Location·Relocation provider exact interface](../../common/spec/server/languages/node/interfaces/08-location-maintenance.ko.md)와
[Location 운영 조회와 observability exact interface](../../common/spec/server/languages/node/interfaces/03-location-observability.ko.md)를
참고한다.
