---
title: "가이드 홈 · C++"
---

# ZLink Framework C++ — 사용자 가이드

<!-- language-switch:start -->
다른 언어로 보기 — **C++** · [C#/.NET](../../../dotnet/guide/server/README.ko.md) · [Java](../../../java/guide/server/README.ko.md) · [Kotlin](../../../kotlin/guide/server/README.ko.md) · [Node/TypeScript](../../../node/guide/server/README.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

**실시간 메시징이 중요한 서버 시스템**을 여러 프로세스로 나눠 만드는 C++
애플리케이션 프레임워크다.

```cpp
#include <zlink/framework.hpp>

int main (int argc, char **argv)
{
    auto app = zlink::framework::app_t::create ();
    app.add_zlink_framework ([] (zlink::framework::zlink_framework_options_t &options) {
        options.http ()
          .listen ("http://0.0.0.0:8080")
          .map_post<open_conversation_http_handler_t> ("/conversations");
    });
    return app.run (argc, argv);
}
```

핸들러 클래스 하나를 등록하면 메시지 디코딩·routing·인코딩은 프레임워크가 처리한다.

---

## 만드는 시스템

여러 서버 프로세스가 역할을 나눠 협력하고, 상태 변화를 실시간으로 클라이언트에
전달해야 하는 시스템에 맞게 설계됐다.

| 도메인 | 핵심 시나리오 |
|--------|--------------|
| **실시간 게임** | 룸 생성 → 플레이어 입장 → 게임 상태 갱신 → 클라이언트 push |
| **고객 지원 채팅** | 대화 개설 → 상담원 배정 → 메시지 중계 → 대화 상태 push |
| **주문 워크플로** | 주문 접수 → 단계별 처리 → 상태 변경 → 클라이언트 알림 |
| **배송·배차** | 배차 요청 → 수행자 배정·수락 → 상태 추적 → 실시간 push |

공통 구조는 하나다 — 역할별 서버 프로세스가 typed 메시지로 통신하고, 클라이언트는
실시간 연결(stream)로 상태 변화를 받는다.

<iframe class="zlink-diagram" src="/common/diagrams/guide-topology.html"
        title="역할별 서버가 typed 메시지로 통신하고, client는 stream으로 받는다" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/guide-topology.html" target="_blank">↗ 크게 보기</a></p>

각 서버 프로세스는 독립 실행 파일이고 서로 TCP로 연결된다. 하나의 서버 안에
HTTP 입구, 다른 서버와의 통신 경로, 클라이언트 연결, 상태 단위 관리가 모두
동거한다. `samples/TicTacToe`(2개 서버)와 `samples/Bingo`(4개 서버)가 동작하는
완전한 예제다.

---

## 핵심 기능

### 채널 메시징 — 서버 간 typed 요청-응답

채널은 서버 사이의 통신 경로에 이름을 붙인 것이다. 한쪽이 채널 이름으로 요청을
보내면 반대편이 처리해 응답한다. struct를 그대로 주고받으며 직렬화(JSON /
MessagePack / Protobuf)는 프레임워크가 처리한다.

```cpp
// 보내는 쪽 (채널 클라이언트)
auto result = co_await _client
    .request ("support.core", open_conversation_req_t{user_id})
    .async<open_conversation_res_t> ();

// 받는 쪽 (채널 서버의 핸들러)
class open_conversation_handler_t {
  public:
    using request_type = open_conversation_req_t;
    using reply_type   = open_conversation_res_t;
    static constexpr const char *topic_name = "OpenConversation";
    open_conversation_res_t handle (const open_conversation_req_t &req) { ... }
};
```

request-reply 외에 fanout(pub/sub)과 route mesh(주소 라우팅) 패턴도 제공한다.
[Channel 메시징 →](20-channel-messaging.ko.md)

---

### SPOT — 상태 단위를 락 없이 관리

SPOT은 게임 룸, 지원 대화, 주문 처리 단위처럼 **"하나의 상태 영역"** 과 그 참여자를
묶는 실행 단위다. 한 SPOT 안에서 일어나는 모든 것 — 참여자 패킷, 타이머, 입퇴장 —
은 **직렬로** 처리된다. std::mutex 없이 상태에 접근할 수 있고, 코루틴으로 비동기
처리를 사용해도 같은 SPOT에 두 요청이 겹치지 않는다.

```cpp
class conversation_spot_t : public zlink::framework::spot_t,
                             public conversation_t   // 대화 상태 직접 소유
{
  public:
    void configure (zlink::framework::spot_context_t &context)
    {
        context.handlers ().add_actor_packet<&conversation_spot_t::send_message> ();
    }

    send_message_res_t send_message (const user_actor_t &actor,
                                     const zlink::framework::message_context_t &,
                                     const send_message_req_t &request)
    {
        return append (actor.user_id, request.text);   // std::mutex 없이 안전
    }
};
```

배정·할당을 담당하는 entry spot(노드당 1개)과 상태 본체인 room spot(단위마다 1개)으로
나뉜다. 주기 작업은 timer로 등록한다. [Spot →](21-spot.ko.md)

---

### Stream + Actor — 클라이언트 실시간 연결

클라이언트의 실시간 양방향 연결을 **stream**이라 하고, 연결 하나를 대표하는
서버 쪽 객체가 **actor**다. 클라이언트가 접속하면 session이 actor를 생성하고,
actor는 SPOT에 입장해 상태 처리에 참여한다.

```cpp
class support_session_t : public zlink::framework::packet_stream_session_t {
  public:
    task_t<void> on_packet (stream_t &stream,
                            const stream_dispatch_context_t &dispatch,
                            const zlink::message_t &payload) override
    {
        auto actor = co_await _actors.find (actor_id);
        co_await actor.value ().relay (payload);   // 현재 dispatch의 packet을 actor로 전달
    }
};
```

클라이언트 쪽 접속은 별도 산출물인 stream connector가 담당한다.
[8장 →](24-actor-session.ko.md) · [9장 →](23-stream.ko.md)

---

### HTTP Hosting — 서버 프로세스 안에 REST API 내장

별도 웹 서버 없이 같은 프로세스 안에 REST endpoint를 올린다. 경로 파라미터,
middleware/handler에서 인증 로직을 구현할 수 있고 TLS를 지원하며 readiness / liveness / health check endpoint도 한 줄로
등록한다.

```cpp
options.http ()
  .listen ("https://0.0.0.0:8443")
  .configure_tls ([] (auto &tls) {
      tls.certificate_file (cert_path).private_key_file (key_path);
  })
  .map_post<create_game_http_handler_t> ("/games")
  .map_get<get_room_http_handler_t> ("/rooms/{room_id}")
  .map_readiness ("/ready");
```

[20장 →](42-http-hosting.ko.md)

---

### Configuration · DI · Logging · Monitoring

운영 서버에 필요한 부속을 내장한다.

- **Configuration** — CLI 인자, 환경 변수, JSON 파일을 우선순위 순서로 합성.
  `bind<T>()` 한 번으로 설정 섹션을 struct에 매핑한다.
- **DI 컨테이너** — 핸들러 생성자 매개 변수만으로 서비스가 자동 주입된다. singleton /
  scoped / transient 수명을 지원한다.
- **Logging** — `logger_t<TOwner>` DI로 받아 소스 이름이 자동 태그된 로그를
  남긴다.
- **Monitoring / Health** — socket·discovery·spot·타이머 이벤트를 typed 구독으로
  받는다. `/ready`, `/healthz` endpoint에 health check를 연결한다.

[18장 →](40-di-container.ko.md) · [19장 →](41-configuration.ko.md) · `11. Monitoring` 장

---

### Registry / Discovery — 서버 주소 자동 연결

여러 Play 서버 인스턴스가 뜰 때 어느 서버로 연결할지, endpoint를 코드에
하드코딩하지 않는다. Redis 기반 공용 Location Store가 주소를 관리하고, 각 서버는
`add_location_store<redis::redis_location_store_t> ()`로 동적으로 찾는다.

```cpp
options.add_location_store<redis::redis_location_store_t> ()
  .set_connection_string (topology.redis_endpoint)
  .set_key_prefix (topology.redis_key_prefix + "location:");

auto room_mesh = options.add_route_mesh (sample_names_t::room_spot_mesh);
room_mesh.set_routing_id (zlink::routing_id_t::from ("bingo-play-" + topology.play_node))
  .listen (topology.selected_play_spot_router_endpoint ());
room_mesh.objects ()
  .server ()
  .add_entry_spot<bingo_entry_spot_t> ()
  .add_spot_factory<bingo_room_spot_t> (sample_names_t::room_spot);
```

[10장 →](25-location.ko.md)

---

## 목차

| 순서 | 문서 | 내용 |
|----|------|------|
| 1 | [개요](01-overview.ko.md) | 핵심 표면, 계층 구조, topology를 빠르게 파악하기 |
| 2 | [퀵스타트](../../quickstart.ko.md) | 설치, 두 process가 서로 호출하는 최소 project, 첫 실행 점검 |
| 3 | [핵심 개념](03-concepts.ko.md) | channel·Spot·Actor·session이 각각 무엇인가 |
| 4 | [Channel 메시징](20-channel-messaging.ko.md) | 이름으로 부르는 경로 — 등록과 호출 |
| 5 | [Spot](21-spot.ko.md) | 여럿이 함께 사용하는 자리를 id로 만들고 부르기 |
| 6 | [Actor](22-actor.ko.md) | 개체 하나를 id로 만들고 부르기 |
| 7 | [STREAM](23-stream.ko.md) | mesh 밖의 client가 연결 하나로 붙기 |
| 8 | [Session과 Actor 연결](24-actor-session.ko.md) | 연결 하나를 Actor 하나에 묶기 |
| 9 | [Location](25-location.ko.md) | id로 지금 있는 node를 조회하기 |
| 10 | [모니터링](26-monitoring.ko.md) | 기능별 가이드 자리 표시 — 본문은 아직 없다 |
| 11 | [실행 모델](32-execution-model.ko.md) | 두 queue, 직렬화 범위, 실행권 |
| 12 | [Backpressure](33-backpressure.ko.md) | 처리보다 도착이 빠를 때와 영향을 주는 옵션 |
| 13 | [활성화와 수명](34-activation-lifetime.ko.md) | 종류별 생성 시점, lifecycle callback, 주입 수명 |
| 14 | [Actor membership](35-actor-membership.ko.md) | Spot 사이 이동, 예약과 상한 |
| 15 | [Timer와 worker](36-timer-worker.ko.md) | 주기 실행, 줄 밖 실행, 실행권 반납 |
| 16 | [Relocation](37-relocation.ko.md) | 옮겨도 남는 것, adapter, 이동 단위 |
| 17 | [Channel 동작 원리](30-channel-patterns.ko.md) | 패턴 차이, 대상 선택, pub/sub, 연결과 discovery |
| 18 | [Handler와 메시지 처리](31-handler-dispatch.ko.md) | 등록 변형, filter, codec, handler 종류 |
| 19 | [STREAM의 동작 원리](38-stream-boundary.ko.md) | 등록 검증, 오류 귀속, 응답 token, 실행 방식 |
| 20 | [Session 묶음의 동작 원리](39-session-binding.ko.md) | 묶는 개수, 경로 갱신, 끊김 통지, 실패 |
| 21 | [ZLink를 어디에 쓰나](17-alternative.ko.md) | 사용처, 대안 비교, 기술 선택 경계, 라이선스 |
| 22 | [운영과 lifecycle](12-operations.ko.md) | 런타임 메트릭, relocate, drain, readiness 연결 |
| 23 | [Options](16-options.ko.md) | 옵션 목록, 기본값과 바꾸는 시점 |
| 24 | [샘플 고르기](14-samples.ko.md) | 어떤 샘플을 먼저 볼지 고르고 실행하는 방법 |
| 25 | [Bingo 따라 읽기](50-bingo.ko.md) | 인증·매칭·room·timer·multicast·cleanup을 코드 순서로 |
| 26 | [TicTacToe 따라 읽기](51-tictactoe.ko.md) | 수동 연결·수동 등록, 다른 node로의 Actor join |
| 27 | [SupportChat 따라 읽기](52-supportchat.ko.md) | session 하나에 Actor 여럿, metadata relay, idle timer |
| 28 | [DeliveryDispatch 따라 읽기](53-deliverydispatch.ko.md) | one-way send, deadline 기록과 재배정, 고객 push |
| 29 | [ShoppingMall 따라 읽기](54-shoppingmall.ko.md) | owner Instance Spot, replay·다음 단계·expected version |
| 30 | [GameQuest 따라 읽기](55-gamequest.ko.md) | player별 owner, best-effort push와 보정 |
| 31 | [ZoneWorld 따라 읽기](56-zoneworld.ko.md) | capacity placement, 경계 join과 relocation, fanout·관찰 |
| 32 | [E2E 테스트](15-e2e-testing.ko.md) | client library로 시스템 전체를 검증하기 |
| 33 | [주요 타입 사용 색인](13-interface-catalog.ko.md) | 계약 인터페이스를 검증 코드로 색인 |
| 34 | [DI 컨테이너](40-di-container.ko.md) | ZLink host — 수명 3종, 등록, handler 자동 주입 |
| 35 | [Configuration](41-configuration.ko.md) | ZLink host — 설정 소스, 우선순위, section 바인딩 |
| 36 | [HTTP Hosting](42-http-hosting.ko.md) | ZLink host — embedded HTTP server와 route handler |
| 37 | [실행과 설정 모델](43-execution-model.ko.md) | ZLink host — thread 모델과 설정이 만나는 자리 |
| 38 | [모니터링](26-monitoring.ko.md) | 재작성 대기 — 상태 snapshot과 진단 |

파일 번호는 언어에 상관없이 같은 장을 가리키는 식별자다. 1~17장은 다섯 언어가 공유하고,
18~21장은 C++에만 있다 — DI·configuration·HTTP hosting·실행 모델은 .NET이 런타임에서
받는 것을 C++은 프레임워크가 직접 제공하기 때문이다.

이 넷은 기초에 해당하므로, 2·3장을 읽은 뒤 바로 18~21장을 먼저 보고 4장으로 돌아와도 좋다.

---

## 다이어그램 읽는 법

이 가이드의 모든 다이어그램은 같은 시각 언어를 사용한다 — 색이 곧 개념이다.

<iframe class="zlink-diagram" src="/common/diagrams/guide-element-kinds.html"
        title="구성도에 나오는 다섯 가지" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/guide-element-kinds.html" target="_blank">↗ 크게 보기</a></p>

여러 장이 같은 TicTacToe/Bingo 토폴로지를 그리며, 장마다 확대 위치만 바뀐다.

## 관련 문서

- HTTP **client**(요청을 보내는 쪽)는 별도 산출물이다 —
  [zlink::http_client 사용자 가이드](../http-client/README.ko.md)
- 설계 계약(초안)은 [doc/spec/](../../README.ko.md)에 있다.
  가이드와 어긋나면 코드와 spec이 정답이다.

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
