---
title: "Node / TypeScript 바인딩 구현 청사진"
---

<!-- bindings-nav:start -->
[스펙 목록](../README.ko.md) | [이전: Java](../java/README.ko.md) | [다음: Python](../python/README.ko.md)
<!-- bindings-nav:end -->

# Node / TypeScript 바인딩 구현 청사진

> **이 장이 정의하는 것** — Node/TypeScript 라이브러리가 가져야 할
> `contracts`/`runtime` 형태와 패키지 export 경계.

이 문서는 Node/TypeScript 라이브러리가 가져야 할 형태를 정의한다. 모든 클래스나
타입 멤버를 빠짐없이 나열하는 문서는 아니다. 구체적인 공개 계약은
`bindings/node/src/index.ts`가 선언하는 패키지 루트 export, `package.json`
exports, 그리고 생성되는 `.d.ts` 표면이다.

Node/TypeScript 구현체는 소스 패키지 트리, 패키지 export, `.d.ts` 타입, 테스트,
샘플, perf 러너, 런타임 동작이 이 청사진을 따르고 안정적인
`core/include/zlink.h` 기능을 TypeScript-idiomatic API로 매핑할 때 정렬되었다고
본다.

이 README는 `../README.md`의 공통 정책에 정렬된 후의 Node/TypeScript 바인딩
형태를 기술하며, Node 리팩터링 작업의 가이드이기도 하다. 리팩터링 중에는 이
문서를 사용해 각 공개 계약, 런타임 구현, 네이티브 브릿지 헬퍼, 테스트, 샘플,
perf import가 어디에 속하는지 결정한다. Node 바인딩이 정렬되었다고 선언되면
생성된 선언, 패키지 export, 테스트, 샘플, perf, 런타임 동작은 이 문서와 일치한다.

Node 리팩터링은 깨는 정리 작업이다. 리팩터링 이전의 공개 표면을 보존하기 위해
호환성 shim, deprecated wrapper, 중복 생성 경로, 런타임 재export alias를 남기지
않는다.

이 바인딩은 공통 바인딩 아키텍처 맵을 TypeScript 명명 규칙과 함께 따른다. 소문자
`contracts`와 `runtime` 소스 폴더를 사용하고, 무엇이 공개인지는 패키지 export가
결정한다. 대문자로 시작하는 .NET이나 C++ 폴더 이름을 Node 패키지에 그대로 복사
하지 않는다.

Node는 정렬 후 [.NET 디자인 형태](../dotnet/README.ko.md)를 따른다. 네이티브 기반 리소스 동작은
`src/zlink/contracts` 아래의 공개 계약 인터페이스와 타입으로 기술하며, 런타임
구현체는 `src/zlink/runtime` 아래에 두고 패키지 루트 팩토리 함수로 얻는다. 구체 값
클래스, DTO 형태의 객체, enum, literal union, 결과, 에러는 계약 소스에 머문다.

리뷰어가 가장 먼저 읽어야 할 코드는 `src/zlink/contracts` 아래의 공개 계약이며,
[.NET 바인딩 청사진](../dotnet/README.ko.md)이 `Contracts/`에서 시작하는 것과 같다. 런타임 파일은 그 계약을
구현해야 하며, 사용자에게 새로운 동작이 발견되는 자리가 되어서는 안 된다.

같은 아키텍처 맵을 사용하면서도 TypeScript와 Node 관례를 유지한다. 소문자 폴더,
camelCase 메서드, PascalCase 공개 타입, TypeScript에 어울리는 곳에서는 구조적
인터페이스를 사용하고, 더 명확할 때에는 작은 DTO 형태 결과를 plain 객체로
표현하며, 컨슈머 표면으로는 패키지 루트 export를 사용한다. TypeScript 관용
표기가 더 명확할 때 C# 인터페이스 접두사, namespace 케이싱, 파일 이름을 그대로
복사하지 않는다.

| 절 | 다루는 내용 |
|---|---|
| [공개 계약 소스](#공개-계약-소스) | export projection, 계약 소스 위치, 패키지 경계 |
| [저장소 레이아웃](#저장소-레이아웃) | 정렬된 디렉터리 트리와 소문자 폴더 규칙 |
| [API 변경 절차](#api-변경-절차) | 신규 매핑·리팩터 절차, 제거해야 할 단축 경로 |
| [라이브러리 형태](#라이브러리-형태) | 인터페이스 우선 정의가 필요한 리소스·역할 목록 |
| [Contract / Runtime 배치 규칙](#contract--runtime-배치-규칙) | 공개 선언과 런타임 구현의 경계 |
| [계약 카테고리 맵](#계약-카테고리-맵) | 카테고리 → 폴더 매핑 |
| [계약 파일 레이아웃](#계약-파일-레이아웃) | `contracts/` 하위 카테고리별 파일 |
| [런타임 파일 레이아웃](#런타임-파일-레이아웃) | `runtime/` 하위 카테고리별 파일과 정렬 실패 예시 |
| [생성 엔트리 포인트](#생성-엔트리-포인트) | 패키지 루트 팩토리 함수 목록 |
| [함수 이름 규칙](#함수-이름-규칙) | camelCase, 정식 액션 이름, handler 등록 규칙 |
| [정식 인터페이스 규칙](#정식-인터페이스-규칙) | recv 시그니처, builder, `publishAsync` 등 예외 규칙 |
| [공개 엔트리 형태](#공개-엔트리-형태) | 패키지 entrypoint의 도메인별 그룹화 |
| [64-bit byte HWM과 monitoring 계약](#64-bit-byte-hwm과-monitoring-계약) | `bigint` HWM 표현과 monitor snapshot field |
| [필수 기능 범위](#필수-기능-범위) | 정렬 시 보장해야 할 사용자 대면 기능 |
| [Spot Get-Or-Create](#spot-get-or-create) | `getOrCreateSpot` 계약 |
| [Receive와 Subscribe 형태](#receive와-subscribe-형태) | 호출자 제공 저장소와 no-data 구분 |
| [에러와 검증 정책](#에러와-검증-정책) | 검증 시점과 에러 구조화 |
| [성능 정책](#성능-정책) | hot path 제약 |
| [구현 체크리스트](#구현-체크리스트) | 정렬 선언 전 확인 항목과 필수 검증 명령 |
| [Actor 및 Spot Route 결과](#actor-및-spot-route-결과) | route 결과 타입과 Actor 대상 send/request |

## 공개 계약 소스

- 공개 계약 projection: `bindings/node/src/index.ts`, 생성되는 `.d.ts`,
  `package.json` exports.
- 계약 소스: `bindings/node/src/zlink/contracts/`.
- 패키지 projection: 패키지 entrypoint에서 export되고 발행된 TypeScript 정의에
  선언된 심볼.
- 내부 구현: 네이티브 addon 모듈, 비공개 소스 모듈, N-API 핸들, 콜백 트램펄린,
  요청 progress 헬퍼, 컨버터, raw part-loop 헬퍼.
- 패키지 경계: `package.json` exports는 문서화된 공개 entrypoint만 노출한다.
- 문서 역할: 이 README는 형태와 의미적 범위(semantic coverage)를 정의한다. 정확한
  공개 멤버 목록은 패키지 entrypoint와 선언이 소유한다.

소스 파일이나 네이티브 브릿지 모듈로의 deep import는 공개 API가 아니다.

## 저장소 레이아웃

Node/TypeScript 바인딩을 변경할 때 다음 경로를 일관되게 사용한다.

- 공개 entrypoint: `bindings/node/src/index.ts`.
- 계약 소스: `bindings/node/src/zlink/contracts/`.
- 런타임 구현: `bindings/node/src/zlink/runtime/`.
- 네이티브 브릿지/아티팩트: `bindings/node/src/zlink/runtime/native/`,
  `bindings/node/native/`, `bindings/node/prebuilds/`, 그리고 생성되는 런타임
  로딩 코드.
- 생성 산출물: `bindings/node/dist/`. 계약 소스가 아니다.
- Codec package: 제공하지 않는다. Node 바인딩은 raw `Message`와 byte payload API만
  유지한다.
- 테스트: `bindings/node/tests/`.
- 샘플: `bindings/node/samples/`.
- Perf: `bindings/node/perf/`.

- `package.json` exports와 생성되는 `.d.ts` 파일은 공개 entrypoint와 일치해야 한다. deep 소스 import를 공개 API로 문서화하거나 테스트하지 않는다.
- `index.ts`, 발행된 `.d.ts` 파일, `package.json` exports가 계약의 TypeScript 패키지 projection이다.
- `package.json` exports에 의도적으로 나열되지 않는 한 deep 소스 경로를 공개 API로 노출하지 않는다.
- 소스 디렉터리 이름은 소문자를 사용한다. `src/zlink/Contracts`나 `src/zlink/Runtime`을 만들지 않는다. 이런 이름은 공개 deep-import 표면으로 오인될 수 있다.
- `src/zlink/contracts`는 공개 TypeScript 타입, 클래스, 빌더, enum, 에러, 팩토리 반환 계약을 소유한다. 패키지 entrypoint나 런타임 팩토리 모듈이 팩토리 구현을 소유한다.
- `src/zlink/runtime`은 네이티브 기반 런타임 구현, 네이티브 addon 호출, 핸들 owner, 콜백 트램펄린, 요청 progress 헬퍼, 마샬링, 플랫폼 로딩을 소유한다.

다음 트리가 정렬된 구현 구조다.

파일 단위는 `../README.md`의 공통 정책을 따른다. 독립적인 공개 개념이나 긴밀한
operation/모델 그룹마다 파일 하나를 유지한다. 매우 작은 타입 alias, 콜백 타입,
enum 전용 파일, pass-through 헬퍼 모듈은 공개 형태를 읽기 쉽게 만들기 위해
인근 계약 파일로 병합한다.

```text
bindings/node/
+-- src/
|   +-- index.ts
|   +-- zlink/
|   |   +-- contracts/
|   |   |   +-- core/
|   |   |   |   +-- context.ts
|   |   |   |   +-- zlink.ts
|   |   |   |   +-- routing_id.ts
|   |   |   +-- messaging/
|   |   |   |   +-- message.ts
|   |   |   |   +-- received.ts
|   |   |   |   +-- topic_message.ts
|   |   |   |   +-- subscription_event.ts
|   |   |   +-- sockets/
|   |   |   |   +-- socket.ts
|   |   |   |   +-- pair_socket.ts
|   |   |   |   +-- dealer_socket.ts
|   |   |   |   +-- router_socket.ts
|   |   |   |   +-- pubsub_sockets.ts
|   |   |   |   +-- stream_socket.ts
|   |   |   |   +-- socket_options.ts
|   |   |   |   +-- socket_operations.ts
|   |   |   +-- eventing/
|   |   |   |   +-- monitor.ts
|   |   |   |   +-- poller.ts
|   |   |   |   +-- timer.ts
|   |   |   +-- service/
|   |   |   |   +-- spot/
|   |   |   |   |   +-- spot_node.ts
|   |   |   |   |   +-- spot.ts
|   |   |   |   |   +-- actor.ts
|   |   |   |   |   +-- spot_operations.ts
|   |   |   |   |   +-- spot_models.ts
|   |   |   +-- errors/
|   |   |   |   +-- errors.ts
|   |   |   |   +-- results.ts
|   |   +-- runtime/
|   |   |   +-- core/
|   |   |   |   +-- context.ts
|   |   |   |   +-- context_options.ts
|   |   |   |   +-- runtime_info.ts
|   |   |   +-- handles/
|   |   |   |   +-- native_handle.ts
|   |   |   |   +-- lifetime.ts
|   |   |   +-- messaging/
|   |   |   |   +-- message_materializer.ts
|   |   |   |   +-- request_progress.ts
|   |   |   +-- buffers/
|   |   |   |   +-- message_conversion.ts
|   |   |   |   +-- buffer_policy.ts
|   |   |   +-- sockets/
|   |   |   |   +-- socket_base.ts
|   |   |   |   +-- socket_options.ts
|   |   |   |   +-- socket_operations.ts
|   |   |   |   +-- pair_socket.ts
|   |   |   |   +-- dealer_socket.ts
|   |   |   |   +-- router_socket.ts
|   |   |   |   +-- pub_socket.ts
|   |   |   |   +-- sub_socket.ts
|   |   |   |   +-- xpub_socket.ts
|   |   |   |   +-- xsub_socket.ts
|   |   |   |   +-- stream_socket.ts
|   |   |   +-- eventing/
|   |   |   |   +-- monitor_socket.ts
|   |   |   |   +-- poller.ts
|   |   |   |   +-- poll_events.ts
|   |   |   |   +-- timer.ts
|   |   |   +-- options/
|   |   |   |   +-- option_mapping.ts
|   |   |   |   +-- validation.ts
|   |   |   +-- service/
|   |   |   |   +-- spot/
|   |   |   |   |   +-- spot_node.ts
|   |   |   |   |   +-- spot.ts
|   |   |   |   |   +-- actor.ts
|   |   |   |   |   +-- spot_operations.ts
|   |   |   +-- errors/
|   |   |   |   +-- native_errors.ts
|   |   |   +-- native/
|   |   |   |   +-- native.ts
|   |   |   +-- internal/
|   |   |   |   +-- request_pump.ts
|   |   |   |   +-- service_mapping.ts
+-- native/
+-- tests/
+-- samples/
+-- perf/
+-- prebuilds/
+-- dist/
```

패키지 루트 export가 컨슈머 entrypoint이다. 테스트, 샘플, perf는 그 entrypoint나
다른 문서화된 패키지 export에서 import하며 `src/zlink/runtime`, 네이티브 addon
모듈, 생성된 비공개 파일에서 import하지 않는다. 패키지 루트나 생성된 `.d.ts`에
나타나는 심볼이라면, 리뷰어는 `src/zlink/contracts` 아래의 계약 소유자나 패키지
루트 entrypoint를 가리킬 수 있어야 한다.

## API 변경 절차

새 core 기능을 매핑할 때:

1. 올바른 계약 소스 카테고리에 공개 심볼을 추가한다.
2. 패키지 entrypoint, 선언 표면, `package.json` projection을 갱신한다.
3. 네이티브 addon 호출, N-API 핸들, 요청 progress 헬퍼는 비공개 모듈 뒤에 둔다.
4. 클래스, 인터페이스, 타입 alias, literal union, plain 객체 형태 중 일반적인
   TypeScript 사용 방식에 맞는 것을 고른다.
5. 런타임 테스트와 패키지 entrypoint를 대상으로 한 타입-표면 테스트를 추가한다.
6. 샘플과 perf는 공개 import를 통해서만 갱신한다.
7. 생성된 `dist`와 `.d.ts` 산출물이 비공개 브릿지 모듈을 노출하지 않는지
   확인한다.

기존 코드를 이 형태로 리팩터링할 때:

1. 공개 동작 선언을 `src/zlink/contracts/<category>/`로 옮긴다.
2. 네이티브 기반 런타임 구현을 `src/zlink/runtime/<category>/`로 옮긴다.
3. 네이티브 addon 로딩과 N-API 호출은 `src/zlink/runtime/native/` 아래에
   유지한다.
4. 공개 코드의 직접 런타임 생성은 패키지 루트 팩토리나 계약 메서드로 치환한다.
5. 런타임 모듈을 공개 API로 노출하는 호환 export를 제거한다.
6. deprecated wrapper, 중복 오버로드 패밀리, 옛 명명 alias는 shim으로
   유지하지 말고 제거한다.
7. 테스트, 샘플, perf는 패키지 루트에서만 import하도록 갱신한다.
8. 선언을 재생성하고 `dist/index.d.ts`가 런타임 구현 모듈이 아니라 계약 표면을
   담고 있는지 확인한다.

다음의 Node 전용 단축 경로가 제거되어야만 리팩터링이 완료된다. 이 항목들은
선택적인 호환 계층이 아니다.

- `src/zlink/contracts`는 런타임 핸들 모듈을 재export하지 않는다.
- 계약 파일은 공개 서비스 모델을 기술하기 위해 런타임 리소스 클래스를 import
  하지 않는다.
- `runtime/handles/canonical.ts`와 같은 공개 런타임 aggregate가 공개 리소스
  동작의 원천으로 남아 있어서는 안 된다. 그러한 선언을 이름 있는 계약 파일과
  리소스별 이름의 런타임 구현 파일로 분할한다.
- `src/index.ts`는 런타임 구현 모듈이 아니라 패키지 계약 이름과 팩토리를 export
  한다.
- `package.json`은 런타임, 네이티브, 생성, 비공개 소스 subpath를 노출하지 않는다.
- 생성된 선언은 런타임 구현 모듈 경로를 공개 타입으로 언급하지 않는다.

인수인계 작업이라면 짧은 작업 설명만으로 충분하다. 이 README와 `../README.md`에
따라 Node 바인딩을 리팩터링하고, .NET 디자인 형태를 사용하며, TypeScript 명명
스타일을 보존하고, 호환 shim을 제거하고, 이 문서의 검증 게이트를 통과시키면
된다.

## 라이브러리 형태

이 바인딩은 네이티브 백엔드를 가진 TypeScript 패키지처럼 느껴진다.

- 네이티브 기반 리소스 동작 계약은 `src/zlink/contracts` 아래의 공개 TypeScript
  인터페이스다.
- 네이티브 기반 런타임 구현은 `src/zlink/runtime` 아래에 산다. 패키지 export가
  아니며 생성 entrypoint도 아니다.
- 공개 계약 파일은 런타임 파일을 열지 않고도 읽을 수 있어야 한다. 리뷰어는
  `contracts/`만으로 호출 가능한 메서드, 반환값, 생명주기, 에러 동작, 빌더
  형태를 이해할 수 있어야 한다.
- 리소스 계약은 `close()`나 동등한 생명주기 메서드를 노출한다.
- 메시지, routing id, 수신 메타데이터, 토픽 메시지, 스냅샷, 옵션, enum, literal
  union, 에러 같은 값들은 일반적인 TypeScript 관례에 따라 구체(concrete) 또는
  구조적(structural)으로 남는다.
- Operation 빌더는 단계적 네이티브 요청 상태와 multipart 누적을 감추므로 공개
  계약 인터페이스를 사용한다.
- 네이티브 addon 핸들, raw 포인터, 콜백 userdata, request pump, part-loop
  시퀀싱은 절대 노출되지 않는다.

순수 DTO/값 객체에 대해 대칭성만을 위해 인터페이스를 도입하지 않는다.
`Message`, `RoutingId`, `Received`, `TopicMessage`, route 결과, 스냅샷, 옵션
객체, enum, literal union, 에러는 구체 또는 구조적인 공개 값으로 남는다.

아래 네이티브 기반 리소스와 역할은 런타임 클래스를 작성하거나 노출하기 전에 공개
TypeScript 인터페이스를 먼저 정의한다.

- `Context`.
- 소켓 역할: 공통 socket behavior, `PairSocket`, `DealerSocket`, `RouterSocket`,
  `PubSocket`, `SubSocket`, `XPubSocket`, `XSubSocket`, `StreamSocket`.
- eventing 역할: `MonitorSocket`, `Poller`, poll event source, `Timer`,
  `Stopwatch`, `AtomicCounter`.
  `Spot`, `Actor`.
- operation builder: send, routed send, request, reply, publish, channel
  send/request, SPOT send/request/reply, actor create, actor join, actor join
  reply builder.
- callback 역할: stream packet handler, monitor handler, poll handler, SPOT
  dispatch handler, route handler, admission handler.

이 역할을 구현하는 런타임 클래스 이름은 private 또는 unexported여도 된다. 그러나
패키지 루트 팩토리와 생성된 선언은 공개 계약 인터페이스 이름을 사용해야 한다.

perf나 샘플이 네이티브 객체에 더 빨리 접근하도록 문서화되지 않은 deep import
경로에 의존하지 않는다.

## Contract / Runtime 배치 규칙

- export되는 TypeScript 클래스, 인터페이스, 타입 alias, 에러 타입, 빌더 계약은
  `src/zlink/contracts`나 패키지 entrypoint에 속한다.
- export되는 패키지 함수, 정적 헬퍼 타입, 편의 메서드 계약, 빌더 헬퍼 계약은
  호출자가 직접 사용할 수 있을 때 계약 소스에 속한다.
- 팩토리 반환 타입과 호출 가능한 팩토리 시그니처는 공개 계약에 속한다. 팩토리
  구현은 계약 파일이 런타임 구현을 import하지 않도록 패키지 entrypoint나
  런타임 팩토리 모듈에 둔다.
- JavaScript 런타임 구현, 네이티브 핸들 owner, request pump, 콜백 어댑터,
  part-loop 헬퍼는 `src/zlink/runtime`에 속한다.
- N-API 바인딩, 네이티브 addon 핸들, 마샬링 헬퍼, 플랫폼 로딩 코드는
  `src/zlink/runtime/native`에 속한다.
- 패키지 export와 발행된 `.d.ts` 파일은 계약 소스를 projection 해야 하며 런타임
  모듈을 노출하지 않는다.
- 런타임의 구체 클래스는 패키지 루트 팩토리 뒤의 생성 대상이다. 호출자는 런타임
  모듈을 직접 import 하지 않는다.
- `src/zlink/runtime/*`를 `src/zlink/contracts`나 `src/index.ts`에서 export 하지
  않는다. `src/index.ts`는 패키지 루트 팩토리를 와이어링하기 위한 목적으로만
  런타임 모듈을 import할 수 있다. 런타임 구현 타입이 공개 계약을 만족할 수
  있으나, export되는 타입 이름은 계약 소스에서 온다.
- 패키지 루트 팩토리는 계약 반환 타입을 명시적으로 선언한다. 예를 들어
  `createContext(): Context`는 런타임 구현을 인스턴스화하더라도 공개 계약
  타입을 반환한다.

## 계약 카테고리 맵

`src/zlink/contracts`는 패키지 entrypoint와 발행된 TypeScript 선언의 소스 소유
맵이다.

- `core/`: 컨텍스트, 컨텍스트 옵션, routing id, 버전/capability 조회 헬퍼, 유틸리티
  계약.
- `messaging/`: `Message`, 수신 메타데이터, 토픽 메시지, 구독 이벤트, 스트림
  packet 데이터, 빌더 payload 헬퍼.
- `sockets/`: 소켓 동작, 소켓 패밀리, 타입 있는 옵션, request/reply,
  publish/subscribe 표면.
- `eventing/`: monitor, monitor snapshot/event, poller, poll event, timer, 공개
  poll 헬퍼.
- `service/`: SPOT node, SPOT 핸들, 토폴로지 모델,
  Actor 참조, Actor 생명주기, operation 빌더.
- `errors/`: 타입 있는 에러 클래스 또는 태그된 에러 도메인.
- enum, flag, result, literal-union 타입은 그 의미를 정의하는 카테고리에 산다.
  단순히 문법으로 묶기 위해 `enums` 폴더를 만들지 않는다.

## 계약 파일 레이아웃

계약 소스는 [.NET 바인딩 청사진](../dotnet/README.ko.md)과 같은 분류를 TypeScript 명명으로 유지한다.
.NET 바인딩을 아는 개발자가 같은 공개 개념을 Node에서 빠르게 찾을 수 있도록
동일한 개념적 파일 그룹화를 유지한다. 폴더 맵은 .NET과 공유하되, 그 안의 이름은
TypeScript 관용 표기를 유지한다.

- `core/`: `context.ts`, `zlink.ts`, `routing_id.ts`, core 옵션/값 파일.
- `messaging/`: `message.ts`, `received.ts`, `topic_message.ts`,
  `subscription_event.ts`, 공통 operation payload 타입.
- `sockets/`: 소켓 인터페이스, 소켓 옵션 타입, send/request/reply 빌더 계약,
  스트림 packet handler 계약, 소켓 플래그.
- `eventing/`: monitor, monitor event/status, poller, poll events, timer, 이벤트
  handler 계약.
- `service/`: SPOT node, Spot, Actor, topology model, service operation builder를
  담는 `spot/` 하위 폴더를 사용한다. `spot_node.ts`, `spot.ts`,
  `actor.ts`, `spot_operations.ts` 같은 이름 있는 파일을 사용하고, 모델 파일은
  해당 서비스 도메인과 함께 묶는다.
- `errors/`: 공개 에러 클래스, result 도메인, 에러 코드 매핑.

공개 리소스 동작을 하나의 종합 `models.ts`나 런타임-export barrel에 모으지
않는다. 작은 DTO 형태 객체와 literal union은 의미를 부여하는 계약과 함께 묶을
수 있으나, 네이티브 기반 리소스와 operation 빌더는 이름 있는 계약 파일이
필요하다.

## 런타임 파일 레이아웃

런타임 소스는 [.NET 바인딩 청사진](../dotnet/README.ko.md)의 런타임 분류를 따라가되 구현만 담는다. Node
런타임 파일 이름은 계약 트리와 같은 소문자 TypeScript 개념 이름을 사용한다.
`default_context.ts`나 `default_pair_socket.ts`와 같이 `default_` 접두사를 쓰는
파일명을 사용하지 않는다. 이 패키지에서는 `src/zlink/runtime` 아래 모든 파일이
이미 계약/런타임 분리에서 네이티브 기반 구현 쪽이다. 파일 이름은 구현체라는
사실이 아니라 구현하는 리소스나 operation을 설명해야 한다.

- `core/`: `context.ts`, `context_options.ts`, 버전/capability 조회 wrapper와 같은
  런타임 헬퍼 함수.
- `handles/`: 네이티브 핸들 owner, 생명주기 검사, close/dispose 상태, reference
  tracking.
- `messaging/`: 메시지 materialization, 요청 progress, 요청 실행, multipart
  progress 헬퍼.
- `buffers/`: 메시지 변환, 버퍼 소유권, copy/borrow 정책, pooled/pinned storage
  헬퍼.
- `sockets/`: `socket_base.ts`, `socket_options.ts`, `socket_operations.ts`, 그
  리고 소켓 패밀리별 구현 파일 한 개씩 — `pair_socket.ts`, `dealer_socket.ts`,
  `router_socket.ts`, `pub_socket.ts`, `sub_socket.ts`, `xpub_socket.ts`,
  `xsub_socket.ts`, `stream_socket.ts`.
- `eventing/`: `monitor_socket.ts`, `poller.ts`, `poll_events.ts`, `timer.ts`,
  관련 이벤트 materialization 헬퍼.
- `options/`: context, socket, service가 공유하는 option 검증과 native option
  id/value 매핑.
- `service/`: SPOT node, Spot, Actor, 토폴로지, 서비스 operation 구현. 구현이
  충분히 커지면 `spot/` 하위 폴더를 사용한다.
- `errors/`: 네이티브 에러 변환과 검증 헬퍼.
- `native/`: 네이티브 addon 로딩, 플랫폼 lookup, N-API 바인딩 표면.
- `internal/`: 표준 .NET 런타임 분류에 맞지 않는 작은 비공개 glue만 둔다. 표준
  분류가 있는 handle ownership, buffer policy, option mapping, native declaration,
  공개 리소스 동작을 여기에 두지 않는다.

런타임 파일은 계약 타입을 import할 수 있으나, 계약 파일은 런타임 파일을 import
하지 않는다. 패키지 루트는 팩토리에서 네이티브 기반 런타임 구현을 인스턴스화할 수
있으나, 런타임 구현 모듈이 아니라 계약 이름을 export한다.

`runtime/sockets/sockets.ts`, `runtime/service/service.ts`,
`runtime/eventing/eventing.ts`, `runtime/core/index.ts` 같은 카테고리 파일은
작은 barrel로만 허용된다. 인근 구현 파일을 재export하거나 런타임 내부에 머무는
팩토리 와이어링을 정의할 수 있으나, 네이티브 기반 리소스 클래스 본문, operation
빌더, 마샬링 로직을 담지 않는다. 리뷰어가 `RouterSocket`, `SpotNode`, `Poller`의
동작을 이해하려고 카테고리 aggregate를 읽어야 한다면 파일 분할이 정렬되지 않은
것이다.

런타임 구현 파일의 이름은 네이티브 기반 구현이라는 사실이 아니라 구현하는 리소스나
operation을 따라 짓는다. `router_socket.ts`, `spot_node.ts`, `poller.ts`,
`timer.ts`를 사용하며, `default_router_socket.ts`, `default_spot_node.ts`,
`default_poller.ts` 등의 이름은 사용하지 않는다.

공유 헬퍼는 두 번째 공개 구현 aggregate가 되어선 안 된다. `runtime/internal/*`은
여러 런타임 분류를 가로지르는 좁은 private glue를 소유할 수 있으나, 공개 리소스의
동작을 소유하거나 표준 .NET 런타임 분류를 숨기면 안 된다. 네이티브 핸들 소유권은
`runtime/handles`, 버퍼 변환은 `runtime/buffers`, option mapping은
`runtime/options`, 네이티브 addon 선언은 `runtime/native`, 공개 리소스 동작은
`sockets/router_socket.ts`나 `service/spot/spot_node.ts` 같은 리소스 런타임 파일에
속한다.

카테고리 아래의 공유 헬퍼 파일도 같은 규칙을 따른다. `runtime/sockets/socket_common.ts`
같은 파일은 좁은 소켓 헬퍼 타입이나 비공개 base 유틸리티를 담을 수 있으나, 여러
무관한 관심사를 한 번에 담지 않는다. operation 빌더, monitor socket 동작,
라우팅 헬퍼, 마샬링 헬퍼, 구체적인 리소스 동작이 한 파일에 함께 들어 있다면 그
파일은 숨겨진 aggregate가 된 것이며 `socket_base.ts`, `socket_options.ts`,
`socket_operations.ts`와 더 작은 내부 헬퍼로 분할해야 한다.

다음 형태들은 명시적인 정렬 실패다.

- `runtime/service/service.ts`가 `SpotNode`, `Spot`, `Actor` 구현을 한 파일에 담은 경우.
- `runtime/eventing/eventing.ts`가 monitor socket, poll events, poller, timer,
  stopwatch, counter 구현을 한 파일에 담은 경우.
- `runtime/core/context.ts`가 context, context options, 무관한 런타임 헬퍼 구현을
  한 파일에 담은 경우.
- `runtime/core/runtime_info.ts`가 헬퍼 함수에 도달하기 위해 복사된 구현
  prelude나 소켓/서비스 동작을 담은 경우.
- `runtime/sockets/socket_common.ts`가 한 거대한 파일에 operation 빌더, monitor
  socket 동작, route 헬퍼, 메시지 변환, base 소켓 동작을 모두 담은 경우.
- `runtime/internal/*`이 비공개 헬퍼 메커니즘 대신 공개 리소스 동작을 소유한
  경우.
- `runtime/internal/*`이 .NET 표준 런타임 분류에 속해야 하는 handle lifetime,
  buffer conversion, option mapping, native addon declaration, error mapping을
  소유한 경우.

## 생성 엔트리 포인트

인터페이스는 동작을 정의하고, 생성은 패키지 루트 팩토리와 공개 계약 메서드가
제공한다.

- `createContext()`는 런타임 컨텍스트 구현을 생성한다.
- `Context.createPairSocket()`, `createDealerSocket()`,
  `createRouterSocket()`, `createPubSocket()`, `createSubSocket()`,
  `createXPubSocket()`, `createXSubSocket()`, `createStreamSocket()`는 런타임 소켓
  구현을 생성한다.
  서비스 계층 구현을 생성한다.
- `Spot` 핸들은 `SpotNode.createSpot()`, `entrySpot()`,
  `getOrCreateSpot(...)`, `spotLookup(...)`을 통해 얻는다. 직접적인 `Spot`
  생성은 공개되지 않는다.
- Actor 핸들은 `SpotNode.createActor(...)`를 통해 생성한다. 직접적인 Actor 생성은
  공개되지 않는다.
- `createPoller()`, `createTimer()`, `createTimer(spot)`은 eventing 리소스를
  생성한다.
- 버전, capability 조회, strerror, proxy, sleep, multipart cleanup 헬퍼 같은 패키지
  루트 팩토리/헬퍼 함수는 공개 계약 함수다. 이 함수들 뒤의 네이티브 호출은 런타임
  모듈에 머문다.

네이티브 기반 런타임 클래스를 직접 생성하는 것은 정렬된 계약의 일부가 아니다.
팩토리가 안정적인 생성 표면이다.

## 함수 이름 규칙

함수 이름은 `../README.md`의 공통 바인딩 의미 규칙을 따르되 TypeScript 표기를
사용한다.

- 메서드와 함수에는 `camelCase`를 사용한다.
- 케이스만 다를 뿐 다른 바인딩과 동일한 정식 액션 이름을 사용한다.
  `send`, `request`, `reply`, `publish`, `subscribe`, `unsubscribe`,
  `recv`, `recvRouted`, `receiveSubscriptionEvent`, `setSendReadyHandler`,
  `setPacketHandler`, `setDispatchHandler`, `getOrCreateSpot`,
  `sendToChannel`, `requestToChannel`, `sendToSpot`, `requestToSpot`.
- 호환성만을 위해 옛 alias를 유지하지 않는다. 리팩터링 이전 이름이 정식 의미와
  충돌하면 제거하고 정식 TypeScript 이름을 노출한다.
- 핸들러 등록에 `on...` 이름을 사용하지 않는다. API가 현재 핸들러를 저장하거나
  교체할 때는 `set...Handler`를 사용한다.
- `sendNoWait`, `publishWithFlags`, `requestAsync` 같은 operation-start 변형을
  만들지 않는다. operation 이름은 하나로 유지하고 flag, timeout, callback, 비동기
  submit 선택은 빌더에 둔다.

## 정식 인터페이스 규칙

- 데이터 평면의 `recv`, routed recv, `subscribe`, 구독-이벤트 receive는 호출자가
  제공한 `Received`, `TopicMessage`, `SubscriptionEvent` 객체를 채우고
  `boolean`을 반환한다.
- send, routed send, publish, request, reply, SPOT operation, Actor location/
  session operation은 fluent 빌더를 반환한다.
- 빌더 시작 메서드는 대상 identity, topic, channel, routing id, 요청 sequence만
  받는다. payload, flag, timeout, callback, 비동기 submit 선택은 빌더 단계다.
- SPOT channel 대상 operation은 `sendToChannel(...)`과
  `requestToChannel(...)`을 사용한다. SPOT topic publish는 `publish(topic)`으로
  유지한다.
- operation 시작 메서드와 같은 이름의 단일 payload 단축 오버로드를 추가하지
  않는다. `send(message)`, `send(routingId, message)`,
  `publish(topic, message)`, `sendToChannel(channel, message)`,
  `sendToSpot(..., message)`은 공개 계약 멤버가 아니다. 호출자는
  `send(...).message(message).submit()`을 사용한다.
- multipart payload는 `message(...)` 반복 호출로 누적한다. `messages(...)`
  편의는 같은 빌더 계약에 위임하고 계약 소스에 선언될 때 허용된다.
- Dealer 소켓은 `requestFrame(...)`이나 `reply(requestToken, parts)` 같은
  프로토콜 envelope 헬퍼를 노출하지 않는다. dealer는 `request()`로 요청을 시작할
  수 있으나 API 수준의 피어 routing id가 없으므로 임의 token에 reply 할 수 없다.
- Node `Buffer` / `Uint8Array` payload 입력은 네이티브 큐가 호출보다 오래
  살아남기 전에 메시지 소유의 네이티브 저장소로 복사된다. `socketSendBorrowedNoWaitResult`
  같은 borrowed Buffer send 헬퍼를 노출하거나 사용하지 않는다.
- 메시지 payload 팩토리는 `Message.from(...)`을 사용한다. 공개 TypeScript 계약은
  호출자가 payload 생성을 위해 `new Message(...)`를 사용하도록 요구하지 않는다.
- operation-start 명명은 위의 함수 이름 규칙을 따른다. 빌더의 종단 메서드는
  Promise 반환 표면에서도 지금처럼 `submit(...)`을 사용한다. `submitAsync` 같은
  별도 종단 이름을 추가하지 않는다.
- MeshNode의 Logical Multicast publisher는 Core의 한 번의 blocking publish를 Node.js
  event loop 밖에서 실행해야 하므로 `publishAsync(...)`를 함께 제공한다. 이 이름은
  이 publisher에만 적용하며 다른 binding operation의 async suffix 규칙을 바꾸지
  않는다. payload와 metadata는 worker를 queue에 넣기 전에 binding이 소유한
  storage로 복사한다. `AbortSignal`은 Core 호출이 시작되기 전까지만 operation을
  취소할 수 있다. Core 호출이 시작된 뒤의 abort는 이미 시작한 publish의 정상 submit
  result와 detail을 바꾸지 않는다. programming 또는 system failure는 예외로 유지한다.
  별도 timeout option은 추가하지 않으며 Core의 MeshNode send timeout을 사용한다.
- `publishAsync(...)`는 `Promise<MeshPublishResult>`를 반환한다. `Ok`,
  `Backpressured`, `NotFound`, `NotConnected`, `Terminated`, `NotAdmitted`는
  정상 submit 결과로 반환하며 Core가 채운 detail을 그대로 보존한다. 특히 일부
  target만 수락한 `Backpressured` 결과의 non-zero detail을 버리지 않는다.
  `InvalidArgument`, `InvalidHandle`, `InvalidState`, `NotSupported`,
  `ThreadViolation`, `OutOfMemory`, `SeqExhausted`, `InternalError`는
  programming 또는 system error이므로 `SubmitError`를 발생시킨다.
- `publishAsync(...)`가 queue에 들어간 뒤 publisher를 `close()`하면 새 publish는
  즉시 거부한다. `close()`는 Node.js event loop를 기다리게 하지 않는다. 이미
  queue에 들어갔거나 Core 호출을 시작한 operation은 native publisher handle을
  유지하며, 마지막 operation의 Core 호출과 Promise 완료 처리가 끝난 뒤 native
  handle을 해제한다.
- MeshNode에서 Actor의 bound session으로 보내는 `sendActorBoundSession(...)`은
  0보다 큰 `expectedBindingGeneration`을 필수로 받는다. binding이 교체된 뒤 이전
  generation의 호출을 새 session으로 전달하지 않으며, 0은 current binding을
  자동 선택하지 않고 Core의 `InvalidArgument` 결과를 보존한다.

## 공개 엔트리 형태

패키지 entrypoint는 API를 도메인 개념을 중심으로 그룹화한다.

- Core: 컨텍스트, 버전/capability 조회 헬퍼, 옵션, 유틸리티 함수.
- Messaging: `Message`, routing id 값, 수신 메타데이터, 토픽 메시지, 구독
  이벤트, 스트림 packet 데이터.
- Sockets: pair, dealer, router, pub, sub, xpub, xsub, stream, 타입 있는 옵션,
  callback, request/reply, publish/subscribe, 스트림 packet API.
- Eventing: monitor, monitor snapshot/event, poller, poll event, timer.
- Service: SPOT node, SPOT 핸들, 토폴로지 스냅샷, Actor
  참조, Actor 생명주기, operation 빌더.
- Errors: 타입 있는 에러 클래스 또는 core result 도메인을 보존하는 태그된 에러
  객체.

## 64-bit byte HWM과 monitoring 계약

HWM과 Auto HWM planning unit은 `uint64_t` byte 값을 손실 없이 표현해야 하므로 공개
TypeScript 타입으로 `bigint`를 사용한다. `number`를 함께 받거나 안전한 정수 범위에 따라
표현을 바꾸지 않는다. `0n`은 HWM에서 무제한을 뜻하며, 수동 HWM 기본값은
`4_096_000n` bytes다. 음수나 `2n ** 64n - 1n`을 넘는 값은 `RangeError`, `number`와
그 밖의 타입은 `TypeError`로 거부한다.

```ts
interface ContextOptions {
  autoHwmMsgUnitBytes: bigint; // 64-bit planning-unit bytes; 0n selects the socket default.
}

interface CommonSocketOptions {
  sendHwm: bigint; // Directional send-pipe byte HWM; 0n means unlimited.
  recvHwm: bigint; // Directional receive-pipe byte HWM; 0n means unlimited.
}
```

Monitor snapshot은 Core monitoring ABI v2를 그대로 투영한다. Planned, applied, deferred와
in-flight HWM 값은 이름에 `Bytes`를 포함하고 `bigint`로 제공한다. Deferred 값의 유효
여부는 별도 boolean으로 제공한다. Pending message와 profile slot은 count 진단값이며 byte
field와 이름을 공유하지 않는다. 이전 `autoHwmAppliedSndHwm` 같은 count 이름은 alias로
유지하지 않는다.

## 필수 기능 범위

공유 .NET-기준 정책에 정렬되었을 때 공개 entrypoint는 다음의 안정적인 사용자
대면 기능을 모두 포함한다.

- 컨텍스트 생명주기, 옵션, shutdown, auto-HWM 재계산, 버전, capability 조회,
  strerror.
- 메시지 ownership, multipart payload, routing id, 수신 메타데이터, 토픽 메시지,
  구독 이벤트, 스트림 packet 콜백.
- 모든 소켓 패밀리와 그 타입 있는 옵션.
- Monitor, poller, timer, readiness 의미.
- SPOT node, SPOT 핸들, 토폴로지 스냅샷, Actor, 스트림
  Actor 바인딩.

바인딩은 적절할 때 동기 또는 비동기 형식을 노출할 수 있으나, core operation의
의미를 바꾸지 않는다.

## Spot Get-Or-Create

Node는 `SpotNode.getOrCreateSpot(spotRid)`를 노출한다. 이는
`zlink_spot_node_spot_get_or_new(...)`에 직접 매핑되며, `spotLookup`과
`createSpot`을 조합해 구현하지 않는다.

이 메서드는 `{ spot, created }`를 반환한다. 반환된 `Spot`은 호출자 소유이며
일반적인 방식으로 close 한다. `created`는 논리적 spot을 생성한 호출에 대해서만
`true`다.

## Receive와 Subscribe 형태

- 데이터 평면 receive와 subscribe API는 재사용 가능한 저장을 위해 호출자가 제공한
  결과 객체를 사용한다.
- 논블로킹 no-data는 `false`를 반환하며 throw된 에러와 구별된다.
- SPOT readable dispatch 이벤트는 readiness 알림이다. 호출자는 일치하는 receive
  API를 no-data가 될 때까지 비운다(drain).
- 네이티브 기반 버퍼는 추가적인 JavaScript 버퍼 연결 없이 소유된 `Message` 객체가
  된다.
- Actor join 요청 receive 같은 서비스 제어/admission receive 경로는 재사용 가능한
  데이터 평면 저장보다 더 명확할 때 nullable, `undefined`, 또는 태그된 결과 반환
  형태를 사용할 수 있다. 그래도 no-data와 throw된 하드 receive 에러는 구별한다.

## 에러와 검증 정책

- 고정 크기 경계 문자열과 id는 네이티브 addon 호출 전에 검증한다.
- routing id, actor id, endpoint, channel 이름, topic을 조용히 잘라내지 않는다.
- submit, request, recv, handler, close, bind, connect, config 에러 도메인을
  보존한다.
- 공개 에러는 호출자가 에러 텍스트를 파싱하지 않고 분기할 수 있을 만큼 구조적인
  데이터를 가진다.

## 성능 정책

- 핫 경로에서는 reflection 식 속성 walking, 문자열 lookup에 의한 동적 dispatch,
  피할 수 있는 할당, 피할 수 있는 `Buffer` 복사, 숨은 sleep, busy wait, 광범위한
  락, worker-thread join을 사용하지 않는다.
- 요청 progress는 outstanding 요청이 있는 동안 네이티브 핸들 단위로 공유한다.
- Poll 결과 materialization은 이벤트별 reflective enum 스캐닝이 아니라 고정 매핑
  테이블을 사용한다.
- Perf, 샘플, 테스트는 공개 패키지 entrypoint만 import 한다.

## 구현 체크리스트

- `package.json` exports는 비공개 모듈을 노출하지 않는다.
- 발행된 `.d.ts` 파일은 공개 계약을 기술한다.
- 네이티브 addon 세부 사항이 공개 타입을 통해 새지 않는다.
- 노출되는 헬퍼 함수와 빌더 편의 메서드는 런타임 헬퍼가 아니라 계약 소스에
  선언된다.
- Receive/구독 의미는 공통 바인딩 정책과 일치한다.
- 데이터 평면 호출자 제공 저장과 다른 서비스 제어/admission receive 예외는 그
  차이가 있는 곳에 문서화된다.
- Perf 의미는 `bindings/c/perf`와 일치한다.
- `src/zlink/contracts`는 `src/zlink/runtime`에 대한 import나 export 의존을 갖지
  않는다.
- `src/index.ts`는 팩토리 와이어링만을 위해 런타임 모듈을 import 하며, 런타임
  모듈이나 런타임 구현 타입 이름을 export하지 않는다.
- 테스트, 샘플, perf는 deep 런타임 import를 사용하지 않는다.
- 네이티브 기반 리소스는 패키지 루트 팩토리나 계약 메서드를 통해 생성되며 계약
  인터페이스 타입으로 표기된다.
- 라이브러리 형태 섹션에 나열된 네이티브 기반 리소스, operation builder,
  callback 역할은 런타임 구현 클래스가 팩토리에 연결되기 전에 공개 계약
  인터페이스를 먼저 갖는다.
- 호환성만을 위한 옛 alias, 중복 operation-start 이름, deprecated wrapper를 남기지
  않는다.

Node 리팩터링 후 필요한 검증. `bindings/node/`에서 다음 명령을 실행한다.

- `npm run build`를 실행한다.
- `npm run typecheck`을 실행한다.
- `npm test`를 실행한다.
- 공개 예제나 생성 경로가 변경되었으면 `npm run samples`를 실행한다.
- 핫 경로, receive, send, request, poller, timer, 서비스 동작이 변경되었으면
  smoke 게이트로 `npm run perf:single`과 `npm run perf:multi`를 실행한다.
- 생성된 선언을 점검해 패키지 루트가 런타임 구현 모듈이 아니라 계약 타입을
  노출하는지 확인한다.
- 공개 표면에서 비공개 import가 있는지 검색한다. 최소한 `src/zlink/contracts`,
  `tests`, `samples`, `perf`에서 `src/zlink/runtime`, `../runtime`, 런타임 핸들
  aggregate, 네이티브 addon 모듈, 생성된 비공개 파일로부터의 import를 확인한다.
  `src/index.ts`는 별도로 점검해 런타임 import가 팩토리 와이어링 전용이고 export
  된 선언에 나타나지 않는지 확인한다.

## Actor 및 Spot Route 결과

Node는 Actor와 Spot route 조회 결과를 공개 JavaScript 객체와 일치하는 TypeScript
선언으로 노출한다.

- `ActorRoute`는 해석된 Actor 참조, Actor 노드 RID, 현재 Spot RID, 현재 Spot
  종류(kind)를 보존한다.
- `SpotRoute`는 Spot RID, owner 노드 RID, Spot 종류를 보존한다.
- `SpotKind`는 Entry Spot과 user Spot을 구분한다. invalid 종류는 성공한 route
  결과가 아니다.
- SpotNode 스냅샷 엔트리는 core 스냅샷과 동일한 Spot kind/현재 Spot 필드를
  노출한다.

- Node는 resolve된 Actor ref를 인자로 받는 `SpotNode.sendToActor(actorRef)`와 `SpotNode.requestToActor(actorRef)`를 노출한다.
- send operation은 submit이 성공하면 하나 이상의 message part 소유권을 넘기고, Actor 소유자 mailbox가 인계를 받으면 완료된다.
- request operation은 submit이 성공하면 요청 part의 소유권을 넘기고, Actor handler가 만든 reply part를 전달한다.
- Node는 제거된 Discovery route table이나 resolver API를 compatibility helper로 되살리면 안 된다.
