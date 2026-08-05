---
title: "11. Monitoring — 상태 관측과 진단 · Node/TypeScript"
---

<!-- framework-adapter-nav:start -->
[가이드 홈](../../../index.ko.md) | [이전: Location](10-location.ko.md) | [다음: 운영 — 메트릭 · drain · readiness](12-operations.ko.md)
<!-- framework-adapter-nav:end -->

# 11. Monitoring — 상태 관측과 진단

> **이 장의 계약 소유 문서** —
> [Node.js location과 observability 공개 계약](../../../common/spec/server/languages/node/interfaces/03-location-observability.ko.md)이
> 다룬다. 이 챕터는 그 계약이 노출하는 네 관측 표면을 사용법 중심으로 설명한다.

handler 호출만으로는 운영을 다 볼 수 없다. 연결이 준비되었는지, 어느 peer가 빠졌는지,
메시지가 어디서 실패했는지도 framework 표면에서 읽어야 한다. Node framework는 이를 **두
갈래**로 노출한다 — 상태 snapshot과 async iterable, 메시지 흐름 기록이다.

runtime event를 provider handler로 받는 표면은 없다. 관측은 전부 아래 두 갈래를 통한다.

## 1. 관측 표면

| 무엇을 보나 | 주입 토큰 | 어디서 다루나 |
|---|---|---|
| Host lifecycle(relocate · drain · readiness) | `ZLINK_FRAMEWORK_RUNTIME` | [12. 운영](12-operations.ko.md) §6.1 |
| MeshNode의 node · peer · channel 준비 상태 | `ZLINK_ROUTE_MESH_RUNTIME` | [12. 운영](12-operations.ko.md) §5 |
| ClientServer channel의 target 상태 | `ZLINK_CLIENT_SERVER_RUNTIME` | 이 챕터 §2 |
| pub/sub channel의 publisher 상태 | `ZLINK_FANOUT_RUNTIME` | 이 챕터 §2 |
| Location store 상태와 topology | `ZLINK_LOCATION_RUNTIME_QUERY` | [10. Location](10-location.ko.md) §4 |
| 메시지 수신 · dispatch · 실패와 흐름 | `configureDispatch()`의 message flow | 이 챕터 §3 |

**전부 `@Inject(토큰)`으로 받는다.** 타입만 적으면 Nest가 무엇을 넣을지 모른다.

## 2. 상태 snapshot과 status stream

상태 표면은 모두 같은 모양이다 — `snapshot(...)`이 지금 값을, `observe(...)`가 그 이후
변화를 **async iterable**로 준다.

```typescript
@Injectable()
export class MeshWatcher {
  constructor(
    @Inject(ZLINK_ROUTE_MESH_RUNTIME) private readonly meshRuntime: ZLinkRouteMeshRuntime
  ) {}

  ready(): boolean {
    const snapshot = this.meshRuntime.snapshot('game.room');  // 지금 값 하나.
    return this.meshRuntime.isReady('game.room');
  }

  async watch(signal: AbortSignal): Promise<void> {
    // capacity를 넘기면 느린 소비자는 중간 값을 건너뛴다.
    for await (const observed of this.meshRuntime.observe('game.room', 64, signal)) {
      // observed.status는 변경 뒤의 완전한 status다. 바뀐 field만 오는 event가 아니다.
      // observed.loss는 이 구독이 놓친 개수다.
      this.record(observed.status);
    }
  }
}
```

**구독을 끝내려면 `AbortSignal`을 끊는다.** `for await` 루프를 `break`로 나가도 되지만,
바깥에서 멈추려면 signal이 유일한 수단이다.

| | `snapshot(...)` | `observe(...)` |
| --- | --- | --- |
| 무엇을 주나 | 호출 시점의 값 하나 | 변경마다 완전한 값 |
| 언제 쓰나 | 운영 endpoint 응답, 단발 확인 | 상태 전이를 기록하거나 반응할 때 |
| 놓칠 수 있나 | 해당 없음 | capacity를 넘기면 중간 값을 건너뛴다 |

Host 상태도 같은 모양이다.

```typescript
for await (const observed of runtime.observe(signal)) {
  const status = observed.status;
  logger.log(`host lifecycle: ${status.state} ${status.relocationResult}`);

  // 이 구독이 놓친 개수. 합치기로 건너뛴 것과 영영 못 보는 것이 나뉘어 있다.
  if (observed.loss.discardedTerminalCount > 0n) {
    logger.warn(`lost terminal statuses: ${observed.loss.discardedTerminalCount}`);
  }
}
```

Peer 상태는 Node RID와 현재 상태, 사용할 수 없는 이유만 담는다. 연결 의도 · discovery
source · lifecycle generation은 framework 내부 상태라 공개하지 않는다.

## 3. 메시지 흐름 추적

메시지 하나가 어디서 어떻게 끝났는지는 message flow로 본다. 수준은 `configureDispatch()`가
정한다.

```typescript
builder.configureDispatch()
  .messageFlow(ZLinkMessageFlowLogMode.ErrorsOnly)   // 기본값 — 실패와 backpressure만.
  .traceLogFile(`${config.logDir}/flow-${config.instanceName}.log`)
  .traceLabel(config.instanceName);
```

| 수준 | 남기는 것 |
| --- | --- |
| `Off` | 남기지 않는다 |
| `ErrorsOnly`(기본) | dispatch 실패와 backpressure |
| `KeyTransitions` | 위 + 수신 · dispatch · 완료 같은 주요 전이 |
| `Verbose` | 위 + 개별 메시지 단위 기록 |

**값이 PascalCase다.** 다른 언어 문서의 `ERRORS_ONLY`를 그대로 옮기지 않는다.

**운영에서는 `ErrorsOnly`로 두고 필요할 때만 올린다.** `Verbose`는 메시지마다 기록을
남기므로 처리량이 많은 구간에서 그 자체가 부하가 된다.

기록을 프로그램에서 받으려면 observer를 등록한다.

```typescript
builder.configureDispatch().setMessageFlowObserver(FlowRecorder);
```

observer는 provider class로 등록한다 — 함수가 아니라 `ZLinkMessageFlowObserver`를
구현한 class다.

## 4. 메트릭

> **Node runtime이 실제로 내는 계기는 계약의 일부다.** 계약은 47개를 정의하는데
> `runtime-metrics.ts`가 선언하는 이름은 44개이고 그중 실제로 기록되는 것은 더 적다.
> 방출 지점이 여러 파일에 흩어져 있어 정확한 수는 확정하지 않았다. **대시보드를 만들기
> 전에 필요한 계기가 실제로 나오는지 확인한다.**

계기 이름 · 종류 · 단위 · label의 계약은
[Runtime metric과 집계 규칙](../../../common/spec/25-runtime-metrics.ko.md)이 소유한다.

## 5. readiness와 liveness

Node에는 별도 health check 표면이 없다. **runtime 상태로 판정한다.**

```typescript
@Controller('healthz')
export class HealthController {
  constructor(
    @Inject(ZLINK_FRAMEWORK_RUNTIME) private readonly runtime: ZLinkFrameworkRuntime
  ) {}

  @Get('ready')
  ready(@Res() res: Response): void {
    res.status(this.runtime.status.isReady ? 200 : 503).send();
  }
}
```

`status`는 **property다** — 호출이 아니다. 다른 언어의 `status()`와 모양이 다르다.

**store 연결처럼 잠깐 끊길 수 있는 의존성은 readiness에만 반영한다.** liveness에 넣으면
store가 잠시 끊겼을 때 오케스트레이터가 프로세스를 죽인다.

## 6. 자주 발생하는 문제

- **`observe(...)` 루프가 안 끝난다** → `AbortSignal`을 넘기고 그것을 끊는다. 루프
  안에서 `break`해도 되지만 바깥에서 멈추려면 signal이 필요하다.
- **상태 전이 일부가 안 보인다** → `observe(...)`의 capacity를 넘겨 건너뛴 것이다.
  capacity를 늘리고 소비를 더 빠르게 한다.
- **`status()`를 불렀더니 오류가 난다** → property다. 괄호를 뺀다.
- **enum 값이 안 맞는다** → Node는 PascalCase다(`ErrorsOnly`).
- **flow 기록이 비어 있다** → 기본 수준이 `ErrorsOnly`라 정상 흐름은 남지 않는다.
- **메트릭이 하나도 안 보인다** → 정상이다. Node runtime은 아직 계기를 내지 않는다(§4).
- **store가 잠깐 끊겼는데 프로세스가 재시작된다** → store 상태가 liveness에 들어가 있다.

## 7. 관련 문서

- 정식 계약: [Node.js location과 observability 공개 계약](../../../common/spec/server/languages/node/interfaces/03-location-observability.ko.md)
- 메트릭과 drain · readiness 운영: [12. 운영](12-operations.ko.md)
- 진단 옵션 목록: [16. Options](16-options.ko.md) §4
- 주입 토큰 목록: [13. 주요 interface 사용 색인](13-interface-catalog.ko.md) §1
