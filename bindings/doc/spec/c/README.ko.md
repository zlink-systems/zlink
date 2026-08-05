---
title: "C 바인딩 구현 청사진"
---

<!-- bindings-nav:start -->
[스펙 목록](../README.ko.md) | [이전: Async & Coroutine Policy](../async-coroutine-policy.md) | [다음: .NET](../dotnet/README.ko.md)
<!-- bindings-nav:end -->

# C 바인딩 구현 청사진

> **이 장이 정의하는 것** — C 바인딩이 `core/include/zlink.h` ABI 위에서
> 갖춰야 할 형태와 리뷰 규칙.

이 문서는 C 바인딩이 갖춰야 할 형태를 정의한다. 모든 공개 함수 시그니처를
다시 복제하지 않는다. 구체적인 공개 API 계약은 `core/include/zlink.h`이다.

C에서는 네이티브 ABI 자체가 바인딩 계약이다. `bindings/c`는 코어 C API 위에
두 번째 계약/런타임 계층을 추가하지 않는다. 공개 헤더, 네이티브 라이브러리
동작, 테스트, 샘플, 패키징, perf 러너가 이 문서와 `core/include/zlink.h`의
규칙에 일치할 때 C 구현이 정렬된 것으로 간주한다.

공유 바인딩 아키텍처 맵은 리뷰 어휘로 여전히 유효하다. core, messaging,
sockets, eventing, service, errors는 리뷰어가 C 헤더를 읽을 때 사용하는
개념적 영역이다. C는 이 영역들을 별도의 `contracts/`와 `runtime/` 폴더가
아니라 헤더 섹션, 타입/함수 접두사, 테스트, 샘플, 문서 섹션을 통해 표현한다.

공유 파일 입도 정책은 C 헤더 섹션과 헬퍼 파일에 대한 리뷰 어휘일 뿐이다.
`core/include/zlink.h`가 ABI 기준선으로 남아 있기 때문에 래퍼 형태의
`contracts/`나 `runtime/` 폴더를 요구하지 않는다.

| 절 | 다루는 내용 |
|---|---|
| [공개 계약 소스](#공개-계약-소스) | 공개 ABI·내부 구현·문서 역할 경계 |
| [저장소 구조](#저장소-구조) | C 바인딩 변경 시 사용하는 경로 |
| [API 변경 절차](#api-변경-절차) | 기능 추가·변경 순서 |
| [라이브러리 형태](#라이브러리-형태) | 네이티브 ABI 함수 네이밍·플래그·멀티파트 규칙 |
| [인터페이스 형태 예외](#인터페이스-형태-예외) | 상위 바인딩 래퍼 규칙이 C에 적용되지 않는 부분 |
| [필수 기능 커버리지](#필수-기능-커버리지) | 리뷰가 점검하는 헤더 기능 그룹 |
| [Spot Get-Or-New](#spot-get-or-new) | `zlink_spot_node_spot_get_or_new` 계약 |
| [Ownership과 생명주기](#ownership과-생명주기) | 핸들·메시지 소유권 이동 규칙 |
| [Error와 Result 정책](#error와-result-정책) | C result 도메인과 예외 미사용 |
| [성능 정책](#성능-정책) | C가 다른 바인딩의 성능 기준선인 이유 |
| [구현 체크리스트](#구현-체크리스트) | 정렬 선언 전 확인 항목 |
| [Actor와 Spot Route 결과](#actor와-spot-route-결과) | route 결과 struct와 라우팅 헬퍼 정책 |

## 공개 계약 소스

- 공개 계약: `core/include/zlink.h`.
- 공개 ABI: 해당 헤더가 선언하는 export된 `zlink_*` 함수, 공개 struct, enum,
  상수, 콜백 typedef, 그리고 ownership 규칙.
- 내부 구현: `core/src/` 아래 파일, 비공개 헬퍼 헤더, 생성된 브리지 파일,
  빌드 스크립트, 테스트 헬퍼.
- 문서의 역할: 이 README는 C 형태와 리뷰 규칙을 기술한다. 정확한 바인딩
  시그니처 목록은 헤더가 소유한다.

`zlink.h` 위에 또 다른 C 파사드를 도입하지 않는다. 단순히 다른 `zlink_*`
함수로 전달만 하는 로컬 alias 함수, 대체 옵션 백, 호환성 래퍼는 바인딩
계약의 일부가 아니다.

## 저장소 구조

C 바인딩을 변경할 때 다음 경로를 일관되게 사용한다.

- 공개 계약: `core/include/zlink.h`.
- 런타임 구현: `core/src/`.
- 네이티브 산출물: `core/build`.
- 바인딩 include 투영: `bindings/c/include/` (패키징이 설치된 헤더를 필요로
  할 때).
- 테스트: `bindings/c/tests/`.
- 샘플: `bindings/c/samples/`.
- perf: `bindings/c/perf/`.
- C API 매핑, 샘플/테스트 지원, perf 정책은 `bindings/c/` 아래에 둔다.

임시 빌드 디렉터리와 생성 출력물은 계약 위치가 아니다.

```text
zlink/
+-- bindings/
|   +-- c/
|   |   +-- include/
|   |   +-- tests/
|   |   +-- samples/
|   |   +-- perf/
+-- core/
|   +-- include/
|   |   +-- zlink.h
|   +-- src/
|   +-- build/
```

## API 변경 절차

C 기능을 추가하거나 변경할 때:

1. `core/include/zlink.h`에 공개 선언을 추가하거나 갱신한다.
2. `core/src/` 아래에 동작을 구현한다.
3. 결과 도메인이 바뀌면 errno/result 문서를 갱신한다.
4. 공개 헤더만 include하는 테스트를 추가한다.
5. 사용자 노출 형태가 바뀐 경우에만 샘플을 갱신한다.
6. 측정 동작이 바뀐 경우에만 perf 러너를 갱신한다.
7. C perf 결과를 해석하기 전에 `core/build`를 재빌드한다.

## 라이브러리 형태

C 바인딩은 네이티브 ABI 형태를 유지한다.

- 함수 이름은 `zlink_*`와 `snake_case`를 사용한다.
- 블로킹/논블로킹 동작은 별도의 `try_*` 공개 함수가 아니라 `ZLINK_DONTWAIT`
  같은 플래그로 선택한다.
- send 경로는 `zlink_submit_result_t` 또는 문서화된 request 결과를 반환한다.
- recv 경로는 `zlink_recv_result_t`를 반환하고 헤더 계약에 따라 호출자
  소유의 출력 저장소를 채운다.
- 멀티파트 페이로드는 반복되는 `zlink_msg_t *part` 호출과
  `zlink_part_flag_t`를 사용한다.
- 라우팅 API는 명시적인 routing id 매개변수와 명시적인 출력 routing id
  저장소를 사용한다.
- 콜백 API는 공개 헤더가 선언한 경우에만 C 함수 포인터와 userdata를
  노출한다.

`Received.Reply(...)`, `Socket.Send().Message(...).Submit()`,
`Spot.Publish(topic)` 같은 상위 수준 객체 편의 표현은 C에 적용되지 않는다.
이런 형태는 상위 수준 바인딩의 것이다.

## 인터페이스 형태 예외

C는 ABI 기준선이며 래퍼 바인딩 인터페이스 규칙을 채택하지 않는다.

- receive와 subscribe는 `zlink.h`가 선언한 출력 매개변수를 사용한다.
- send, publish, request, reply는 명시적 `zlink_*` 함수와 플래그를 사용한다.
- C는 오퍼레이션 빌더, 단계형 인터페이스, 플루언트 헬퍼 객체를 노출하지
  않는다.
- 공개 static 파사드, 빌더 편의 헬퍼, contract/runtime 폴더에 대한 래퍼
  바인딩 규칙은 C에 적용되지 않는다. C 헬퍼가 공개라면 반드시
  `core/include/zlink.h`에 선언한다. 그렇지 않으면 내부 헬퍼다.
- C 샘플과 perf는 공개 헤더를 include하고 공개 C ABI를 직접 호출한다.

## 필수 기능 커버리지

C 리뷰는 `core/include/zlink.h`에서 다음 그룹을 점검한다.

- 런타임, 버전, 기능(capability) 조회, 컨텍스트 생명주기, 컨텍스트 옵션.
- 메시지 생명주기, 메시지 데이터 접근, copy/move/adopt 규칙, 속성 조회.
- 소켓 생명주기, bind/connect, disconnect, 옵션, TLS 헬퍼, routing id,
  send, receive, request, reply, publish, subscribe, stream API.
- 이벤팅 API: monitor, poller, timer, 콜백 등록, readiness 의미.
- SPOT node, SPOT handle, topology snapshot, actor, service 계층 API.
- error/result enum과 errno 매핑.

`core/include/zlink.h`에 기능이 있으면 C 바인딩은 그것을 공개 헤더를 통해
직접 노출한다. 해당 헤더에 없는 기능은 공개 C API가 아니다.

## Spot Get-Or-New

`bindings/c/include/zlink.h`는 코어 공개 헤더와 동일한 시그니처 및 결과
계약으로 `zlink_spot_node_spot_get_or_new(...)`를 노출한다. 이 함수는
routing id로 로컬 논리 Spot을 원자적으로 얻거나 생성하며, 호출자 소유의
`Spot` 파사드 핸들과 생성 여부 플래그를 함께 반환한다.

이 API는 actor를 Spot에 join하지 않는다. join은 별도의 service 작업으로
남는다.

## Ownership과 생명주기

C 호출자는 메모리를 명시적으로 소유한다. 헤더는 모든 경계에서 ownership
이동을 분명하게 드러낸다.

- `zlink_msg_t` 값은 사용 전에 초기화하고 정확히 한 번 close한다.
- 메시지 저장소를 move하거나 adopt하는 API는 호출 후 소스 객체의 상태를
  문서화한다.
- recv API는 호출자가 제공한 저장소를 채운다. 호출자는 자신에게 소유권이
  넘어온 메시지 파트를 close한다.
- 핸들은 대응하는 `zlink_*_close`, `zlink_*_destroy`, `zlink_ctx_term`,
  또는 문서화된 생명주기 함수를 통해 close한다.
- 콜백 등록은 호출자가 비공개 worker, socket, inproc endpoint 세부 정보를
  알도록 요구하지 않는다.

## Error와 Result 정책

C 바인딩은 공개 결과를 예외가 아니라 C result 도메인으로 보고한다.

- 일시적 no-data는 문서화된 recv 결과로 보고한다.
- 일시적 backpressure는 문서화된 submit 결과로 보고한다.
- configuration, bind, connect, close, request, handler, recv 실패는
  `zlink.h`의 result와 errno 규칙에 매핑된다.
- 공개 헤더는 호출자가 실패를 분류하기 위해 비공개 구현 상태를 들여다보도록
  요구하지 않는다.

## 성능 정책

C 바인딩은 다른 바인딩의 성능 기준선이다.

- 핫 패스는 공개 part substrate가 파트를 그대로 스트리밍할 수 있을 때
  aggregate materialization을 추가하지 않는다.
- send/recv, request, dispatch, poller, timer, stream, SPOT, actor 경로에
  숨겨진 sleep, busy wait, thread join, 리플렉션 같은 동적 디스패치, 거친
  글로벌 락, 피할 수 있는 복사를 추가하지 않는다.
- perf 러너와 샘플은 공개 헤더만 include한다.
- `bindings/c/perf`는 perf 정책이 명시적으로 달리 정하지 않는 한
  `core/build`의 런타임을 측정한다.

## 구현 체크리스트

C 바인딩이 정렬되었다고 선언하기 전에:

- `core/include/zlink.h`가 정확한 공개 C ABI를 선언한다.
- 헤더 문서와 errno/result 문서가 일치한다.
- 공개 테스트와 샘플이 비공개 헤더 없이 컴파일된다.
- 상위 수준 바인딩이 비공개 C 헬퍼를 호출하지 않고 자신의 공개 형태를
  구현할 수 있다.
- perf 출력이 런타임 라이브러리 경로를 인쇄하며 오래된 `core/build`
  런타임으로 실행하지 않는다.

## Actor와 Spot Route 결과

C 바인딩은 코어 route 결과 struct를 그대로 공개 ABI로 노출한다.

- `zlink_actor_route_t`는 해석된 Actor ref를 담는다. `actor.node_rid`와
  더불어 `current_spot_rid`, `current_spot_kind`를 포함한다.
- `zlink_spot_route_t`는 요청된 `spot_rid`, `owner_node_rid`, `spot_kind`를
  담는다.
- `zlink_spot_kind_t`는 Entry Spot과 사용자 Spot을 구분한다. 잘못된 kind는
  성공적인 Actor 또는 Spot route 결과가 아니다.
- Actor id로 라우팅하는 C 샘플은 먼저 Actor를 해석한 뒤, `actor.node_rid`와
  `current_spot_rid`를 기존 Spot routed API에 전달한다.

C 바인딩은 `zlink_router_send_actor`, `zlink_router_request_actor`,
또는 Actor-to-ROUTER request 헬퍼를 추가하지 않는다. Actor 지정 전달은
route 조회 뒤 기존 Spot routed send/request로 처리한다.
