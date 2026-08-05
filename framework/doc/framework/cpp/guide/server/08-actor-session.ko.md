---
title: "8. Session과 Actor binding · C++"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/08-actor-session.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: 7. Actor와 Spot](07-actor-spot.ko.md) | [다음: 9. STREAM](09-stream.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C#/.NET](../../../dotnet/guide/server/08-actor-session.ko.md) · **C++** · [Java](../../../java/guide/server/08-actor-session.ko.md) · [Kotlin](../../../kotlin/guide/server/08-actor-session.ko.md) · [Node/TypeScript](../../../node/guide/server/08-actor-session.ko.md)
<!-- language-switch:end -->

# 8. Session과 Actor binding

> **이 장의 계약 소유 문서** — [Session Actor dispatch](../../../common/spec/20-session-actor-dispatch.ko.md)가
> 동작을, [언어별 STREAM session · bound session 공개 계약](../../../common/spec/server/languages/README.ko.md)이
> 정확한 시그니처를 소유한다.

Session binding은 client STREAM session과 exact Actor incarnation을 연결한다. Binding 뒤 session은
client packet을 Actor로 relay할 수 있고, Actor는 같은 session으로 push할 수 있다.

Binding은 Actor의 Spot membership과 독립이다. Actor가 다른 Spot이나 node로 relocation되어도
`ActorId`와 `ObjectGeneration`은 유지되며 Framework가 binding route를 갱신한다.

**개수는 한쪽만 열려 있다.** Session 하나는 여러 Actor를 동시에 bind할 수 있다 — 한
connection이 player Actor와 party Actor를 함께 써도 된다. 반대로 **Actor 하나는 동시에
session 하나에만 bind된다.** 새 binding이 확정되면 이전 binding은 무효가 되고 그쪽으로 온
늦은 message는 거부된다.

**relay는 Location Store를 다시 조회하지 않는다.** bind할 때 확인한 route를 session이
Actor마다 보관하고 그것으로 보낸다. Actor가 옮겨가면 relocation commit 뒤 Framework가 그
보관 route를 갱신한다 — application이 다시 bind하지 않는다.

## 1. 인증 뒤 Actor bind

Session handler에서 Actor를 생성하거나 조회한 다음 `ActorRef`를 bind한다. Local Actor instance나
target `NodeRid`를 직접 전달하지 않는다.

> **샘플에서 보기 — [TicTacToe](../../../common/sample/tictactoe/README.ko.md).** 인증 요청을
> 받아 player Actor를 만들고 session에 bind한 뒤 reply까지 보내는 자리다. 저장소의 실제
> 코드다.

```cpp
--8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Sessions/Handlers/authenticate_play_session_handler.hpp:doc-session-auth"
```

최소 형태로 보면 이렇다.

```cpp
// C++ session은 handler 등록 대신 on_packet에서 직접 분기한다.
auto located = actors.get_or_create (sample_names_t::actor_type, request.player_id,
                                     create_player_t{request.display_name});
if (!located)
    co_return result_t<session_actor_t>::failure (framework_error_kind_t::request_failed,
                                                 "Player actor could not be located.");

// 같은 exact incarnation이 이미 bind됐으면 기존 route를 반환한다.
auto actor = co_await actors.bind_or_get (located.value ().ref ()).submit ();

// 현재 request의 one-shot reply를 제출한다.
stream.reply_packet (zlink::message_t::from_json (authenticated_t{actor.actor_id ()}))
  .submit ();
```

`bind`는 중복 bind를 오류로 처리한다. 인증 재전송처럼 이미 bind됐을 수 있는 흐름은
`bind_or_get`를 사용한다.

## 2. Session packet을 Actor로 relay

Session의 `configure()`에서 인증 같은 session 전용 handler를 등록한다. 처리하지 않은 packet은
bound Actor로 넘긴다.

```cpp
// C++ session은 interface를 상속하고 on_packet 하나에서 분기한다.
// handler registry 대신 "인증 packet인가"를 직접 확인하는 형태다.
class play_session_t : public packet_stream_session_t
{
  public:
    task_t<void> on_packet (stream_t &stream,
                            const stream_dispatch_context_t &dispatch,
                            const zlink::message_t &payload) override
    {
        // Actor binding 전에 처리할 packet을 먼저 걸러 낸다.
        if (_authenticate.can_handle (dispatch)) {
            auto authenticated = co_await _authenticate.handle (_actors, stream, payload);
            _bound_actor_id = std::string (authenticated.actor_id ());
            co_return;
        }

        auto actor = require_bound_actor ();
        if (!actor)
            co_return;

        // decode하지 않고 Framework-owned payload를 Actor handler로 넘긴다.
        co_await actor.value ().relay (payload);
    }

    task_t<void> on_connected (stream_t &) override { co_return; }
    task_t<void> on_disconnected (stream_t &) override { co_return; }
    task_t<void> on_error (stream_t &, const stream_error_t &) override { co_return; }
};
```

한 session에 여러 Actor를 bind할 수 있다. 이 경우 application protocol이 선택한 ActorId를
`Context.Actors.Find(actorId)`에 전달한다. Framework는 임의의 Actor를 선택하지 않는다.

## 3. Disconnect 통지

Physical STREAM disconnect는 Framework가 current binding 전체에 자동으로 통지한다. 연결이 유지된
상태에서 logical disconnect를 알릴 때만 명시적으로 호출한다.

```cpp
if (auto actor = _actors.find (player_id)) {
    // Actor가 속한 Spot의 on_disconnect_actor callback 완료까지 기다린다.
    co_await actor->notify_disconnected ();
}
```

Disconnect는 Actor를 삭제하거나 Entry Spot으로 이동시키지 않는다. 재접속한 session은 같은
ActorRef를 다시 조회해 bind할 수 있다.

**한 Actor의 통지가 실패해도 나머지는 계속한다.** Framework는 연결이 끊긴 시점의 binding
snapshot을 고정하고 각 Actor에 통지하는데, 그중 하나가 실패하거나 callback이 deadline을
넘겨도 남은 Actor 통지와 session cleanup을 멈추지 않는다.

**자동 통지와 명시 호출이 겹쳐도 callback은 한 번만 실행된다.** 같은 binding에 대한 두
통지를 Framework가 합치므로, 명시 호출 직후 연결이 끊겨도 Spot의 disconnect callback이
두 번 돌지 않는다.

## 4. Actor에서 client로 push

Actor handler는 `Context.BoundSession`으로 현재 bound client에 메시지를 보낸다.

```cpp
// C++ actor handler는 Spot member 함수다. Spot은 this로 받으므로 인자는 셋이다.
task_t<void> game_room_t::state_changed (player_actor_t &actor,
                                         const message_context_t &,
                                         const state_changed_t &message)
{
    // 현재 bound session의 local admission까지 기다린다.
    co_await actor.context ().bound_session ()
      .send (game_state_notify_t{message.state})
      .metadata ("revision", std::to_string (message.revision))
      .submit ();
}
```

Bound session은 push와 disconnect만 제공한다. Client request에 대한 Actor reply는 request handler의
반환값으로 처리한다.

## 5. 오류를 처리하는 기준

| 상황 | 결과 |
|---|---|
| Actor가 없거나 Ready가 아님 | bind가 typed framework error로 끝난다. |
| `ObjectGeneration`이 다름 | stale ActorRef를 다른 incarnation에 bind하지 않는다. |
| Actor relocation seal 진행 중 | `ActorMoving`으로 끝나며 hidden retry하지 않는다. |
| Binding 뒤 Actor relocation | Framework가 route를 갱신하며 session을 다시 bind하지 않는다. |
| Session disconnect | Actor와 Spot membership은 유지한다. |
| Session이 닫힌 뒤 도착한 reply | 버린다. 새 session이나 새 binding의 reply로 쓰지 않는다. |
| relay 뒤 timeout · route 실패 | 다른 Actor · 새 owner · 다른 node로 자동 재전송하지 않는다. |

`ActorRef.MeshName`과 `NodeRid`는 최초 control route의 snapshot이다. Application은 stale route를
새로 조합하지 말고 actor manager의 조회 호출로 current ref를 다시 얻는다.

## 6. 관련 문서

- 이 챕터 계약의 실행 검증 예문: [13. Interface 카탈로그](13-interface-catalog.ko.md) §5 — 검증 클래스 `StreamContracts`
- STREAM node와 session lifecycle: [STREAM](09-stream.ko.md)
- Actor 생성과 Spot join: [Actor와 Spot](07-actor-spot.ko.md)
