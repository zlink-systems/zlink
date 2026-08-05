# 01. Host lifecycle

[레퍼런스 목차](README.ko.md)

이 category는 `ZLinkModule`/`zlinkFramework()` 등록 진입점과 `ZLinkFrameworkRuntime`이 제공하는
진입점을 다룬다. 정확한 signature는
[NestJS host adapter exact interface](../../common/spec/server/languages/node/interfaces/07-nestjs-host.ko.md)와
[Location 운영 조회와 observability exact interface](../../common/spec/server/languages/node/interfaces/03-location-observability.ko.md)가
소유한다.

---

## `ZLinkModule.forRoot` (구성 시점)

Framework root를 NestJS application에 한 번 등록한다. 다른 모든 항목의 전제 조건이다.

```ts
@Module({
  imports: [
    ZLinkModule.forRoot({
      ...zlinkFramework()
        .addRouteMesh("play")
        .listen(5501)
        .setRoutingIdPrefix("play")
        .setPlacementWeight(100)
        .build(),
    }),
  ],
})
export class AppModule {}
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `ZLinkModule.forRoot(options: ZLinkModuleOptions)` | 필수 | `zlinkFramework()...build()`가 만든 `ZLinkModuleOptions`를 등록 |
| `ZLinkModule.forRootFactory({ useFactory, inject?, imports? })` | — | Async factory로 옵션을 만들 때 쓰는 overload |
| `zlinkFramework(): ZLinkNestFrameworkOptionsBuilder` | 필수 진입점 | topology, handler, Location Store 등 모든 등록의 fluent builder를 시작한다. 마지막에 `.build()`로 `ZLinkModuleOptions`를 만든다 |

**완료 결과.** 반환값 없이 동기로 등록된다. NestJS module 초기화 시점에 구성을 검증하고, 실패하면
`ZLinkConfigurationException`으로 startup 자체를 실패시킨다 — 잘못된 구성이 message 처리 중에
처음 나타나지 않는다.

**선택 기준.** 모든 host가 정확히 한 번 호출한다. `ZLinkNestFrameworkOptionsBuilder`의 topology·
handler 등록 세부는 topology-discovery category를 참고한다.

---

## `relocate`

현재 host가 들고 있는 stateful object(User Spot·Actor)를 다른 eligible node로 이전한다. 계획된
점검이나 rolling update 전에 호출한다. `ZLINK_FRAMEWORK_RUNTIME` DI token으로 주입받은
`ZLinkFrameworkRuntime`이 제공한다.

```ts
const result = await frameworkRuntime.relocate({
  mode: ZLinkFrameworkRelocationMode.RollingUpdate,
  targetApplicationVersion: 2n,
  deadlineMs: 5 * 60_000,
});

if (result.outcome === ZLinkFrameworkRelocationOutcome.Relocated) {
  await frameworkRuntime.shutdown();
}
```

**옵션.** `ZLinkFrameworkRelocationOptions`의 field는 다음과 같다.

| Field | 기본값 | 의미 |
| --- | --- | --- |
| `mode` | 필수 | `PlannedMaintenance`(source와 같은 application version만 target) 또는 `RollingUpdate`(지정한 version만 target). 생략할 수 없다 |
| `targetApplicationVersion` | `PlannedMaintenance`에서는 지정하면 안 됨, `RollingUpdate`에서는 필수(source보다 커야 함) | 목표 application version. 조합이 맞지 않으면 admission을 변경하기 전에 `TypeError`로 reject된다 |
| `deadlineMs` | Framework 기본 deadline | eligible target 수렴을 기다리는 상한 |
| `signal` | 없음 | 이 `Promise`의 대기만 취소한다. 이미 시작된 shared relocation operation 자체는 취소하지 않는다 |

**완료 결과.** `ZLinkFrameworkRelocationResult.outcome`이 `Relocated`면 모든 object 이전이 끝나고
host는 `Relocated` 상태가 된다(새 operation은 받지 않지만 infrastructure는 유지한다). `Blocked`면
`reason`에 `TargetUnavailable`·`StoreUnavailable`·`DeadlineExceeded` 등이 담긴다.

**선택 기준.** 배포 전 무중단 이전이 필요할 때 쓴다. 이전 없이 바로 종료하려면 `shutdown`을 직접
호출한다. 같은 mode·target version으로 중복 호출하면 진행 중인 operation에 합류하고, 다른 값으로
호출하면 `Blocked/OperationInProgress`로 완료한다.

---

## `shutdown`

Host를 종료한다. Relocation을 시작하지 않는다 — 이전이 필요하면 먼저 `relocate`를 호출한다.

```ts
const result = await frameworkRuntime.shutdown({ deadlineMs: 30_000 });
```

**옵션.** `ZLinkFrameworkLifecycleOptions`의 field는 다음과 같다.

| Field | 기본값 | 의미 |
| --- | --- | --- |
| `deadlineMs` | Framework 기본값 | 종료 정리 상한. 초과하면 `ForceStopped`로 완료한다 |
| `signal` | 없음 | 이 `Promise`의 대기만 취소한다 |

**완료 결과.** `ZLinkFrameworkTerminationResult.outcome`이 `Stopped`(정상 정리) 또는
`ForceStopped`(deadline 초과·정리 실패)다. `Serving`에서 호출하면 남은 application 처리와
resource를 정리하고, `Relocated`에서 호출하면 infrastructure 연결만 정리한다.

**선택 기준.** Host를 종료할 때 항상 호출한다. `Relocating` 도중 호출하면 진행 중인 atomic
relocation unit의 결과만 확정하고 나머지는 시작하지 않는다 — 그 relocation을 기다리던 호출자는
`Blocked/ShutdownRequested`를 받는다.

---

## `status` / `observe` (읽기·관찰)

Host의 현재 상태를 한 번 읽거나, 상태 변화를 실시간으로 관찰한다.

```ts
const status = frameworkRuntime.status;
const canAcceptNewOperations = status.isReady && status.acceptingWork;

for await (const observed of frameworkRuntime.observe()) {
  // observed.status, observed.loss를 확인한다
}
```

**옵션.** `status`는 property(getter)이고, `observe(signal?)`는 `AsyncIterable`을 반환한다 — 둘 다
인자로 받는 modifier가 없다(`observe`는 취소용 `AbortSignal`만 선택적으로 받는다).

**완료 결과.** `status`는 동기 읽기다. `isReady`는 `state === Serving`일 때만 `true`이고,
`acceptingWork`는 새 application operation 수락 여부를 나타낸다 — 두 값이 다를 수 있으므로 둘 다
확인한다. `observe(...)`는 terminal 완료 없이 스트리밍하며, `ZLinkObservedStatus.loss`
(`coalescedCount`/`discardedTerminalCount`)로 관찰 유실 여부를 판단한다. `signal` abort만 해당
iteration을 종료한다.

**선택 기준.** 지금 이 순간의 상태 한 번만 필요할 때 `status`를, 상태 전이를 놓치지 않고 계속
받으려면 `observe(...)`를 쓴다. `ZLinkDrainHealthIndicator`(NestJS `@nestjs/terminus` 연동)도 이
runtime의 RouteMesh 상태를 읽어 readiness probe를 구성한다 — topology-discovery category의 상태
조회 항목을 참고한다.

---

전체 근거는
[NestJS host adapter exact interface](../../common/spec/server/languages/node/interfaces/07-nestjs-host.ko.md)와
[Location 운영 조회와 observability exact interface](../../common/spec/server/languages/node/interfaces/03-location-observability.ko.md)를
참고한다.
