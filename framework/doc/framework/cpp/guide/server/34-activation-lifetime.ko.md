---
title: "활성화와 수명 · C++"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/34-activation-lifetime.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# 활성화와 수명

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: Backpressure — 처리보다 도착이 빠를 때](33-backpressure.ko.md) | [다음: Actor membership](35-actor-membership.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C#/.NET](../../../dotnet/guide/server/34-activation-lifetime.ko.md) · **C++** · [Java](../../../java/guide/server/34-activation-lifetime.ko.md) · [Kotlin](../../../kotlin/guide/server/34-activation-lifetime.ko.md) · [Node/TypeScript](../../../node/guide/server/34-activation-lifetime.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    Spot 종류마다 언제 만들어지고 어떤 callback을 받는지, 그리고 그 안에 주입한 서비스가
    얼마나 사는지 알 수 있다. 이 장의 코드는 TicTacToe 샘플에서 가져왔다.

[Spot](21-spot.ko.md)은 **application이 명시적으로 만드는 Spot**을 다뤘다. 이 장은
나머지 종류와의 차이, 종류마다 받는 lifecycle callback, 그리고 Spot이 살아 있는 동안 유지되는
주입 scope를 다룬다.

## 1. Spot의 종류

Spot은 종류와 무관하게 id와 상태를 가지고 callback을 순서대로 실행한다. 생성 시점, Actor membership,
종료 계약이 다르다.

| | Entry Spot | User Spot | Instance Spot |
| --- | --- | --- | --- |
| 생성 시점 | Object Server가 시작할 때 Framework가 만든다 | application이 spot manager로 명시적으로 만든다 | 그 id로 첫 메시지가 도착할 때 만든다 |
| Spot id | Framework가 발급한다 | 만들기는 Framework가, 찾아서 만들기는 호출하는 쪽이 정한다 | 호출하는 쪽이 메시지의 대상 id로 정한다 |
| stable type | 등록하지 않는다 | 필수다 | 필수다 |
| Actor membership | 지원한다. Actor가 만들어진 직후의 기본 실행 위치다 | 지원한다. Actor가 join·leave로 옮겨 다닌다 | 지원하지 않는다 |
| application이 닫기 | 제공하지 않는다 | 닫는 호출이나 자기 context에서 닫는다 | 자기 handler·timer context에서 닫는다 |
| 주로 사용하는 자리 | 아직 User Spot에 속하지 않은 Actor의 기본 위치 | room, stage, zone | matchmaking worker처럼 id 단위로 요청을 처리하는 단위 |

<iframe class="zlink-diagram" src="/common/diagrams/34-spot-kinds.html" title="세 종류의 Spot — 무엇이 만드는가" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/34-spot-kinds.html" target="_blank">↗ 크게 보기</a></p>

Instance Spot에는 만드는 호출이 따로 없다. 첫 메시지에 instance type을 적으면 Framework가 있던
instance를 고르거나 필요한 위치에 만든 뒤 **그 같은 메시지를 처리한다.**

## 2. 종류마다 받는 lifecycle callback

이름은 언어를 따르고, 호출 조건과 순서는 같다.

| callback | Entry | User | Instance | 언제 |
| --- | :---: | :---: | :---: | --- |
| `configure` | O | O | O | handler를 등록하는 구성 단계 |
| `on_create` | X | O | X | 새 User Spot 생성 요청을 확인하고 받아들일지 정한다. 있던 Spot을 찾은 경우에는 호출하지 않는다 |
| `on_initialize` | O | O | O | 만들어진 instance의 초기화. Instance Spot은 `on_create` 없이 이것만 받는다 |
| `on_closing` | O | O | O | 아직 유효한 local instance가 정리되기 전 |
| `on_actor_join` | X | O※ | X | 이미 있는 Actor가 이 User Spot으로 오려 할 때 승인하거나 거절한다 |
| `on_create_actor` | O※ | X | X | 새 Actor의 최초 Entry Spot membership을 승인하거나 거절한다 |
| `OnJoinedActor` | O※ | O※ | X | join commit이 끝났음을 **간 쪽** Spot에 알린다 |
| `on_leave_actor` | O※ | O※ | X | commit 뒤 **떠난 쪽** Spot에 알린다. Actor가 사라졌다는 뜻이 아니다 |
| `on_disconnect_actor` | O※ | O※ | X | 그 Spot 소속 Actor의 연결이 끊겼을 때 |

※ Actor type을 지정해 Actor membership을 지원하는 Spot에만 해당한다.

**membership callback은 떠난 Spot과 간 Spot에서 나뉘어 실행된다.** 그래서 User Spot에 있던
Actor가 Entry Spot으로 돌아가도 **Entry Spot의 `on_create_actor`와 `on_actor_join`은 호출되지
않는다** — Entry Spot 복귀는 기본 membership이라 승인 절차가 없다. 양쪽 모두 commit 뒤 간 쪽의
`OnJoinedActor`와 떠난 쪽의 `on_leave_actor`만 실행된다.

!!! info "relocation은 들어오고 나간 사건이 아니다"

    Relocation으로 Actor가 다른 node의 Entry Spot에 복원될 때는 이 callback을 호출하지 않는다.
    membership을 그대로 둔 채 실행 위치만 옮기는 것이기 때문이다 —
    [Relocation](37-relocation.ko.md)이 다룬다.

## 3. Entry Spot — Framework가 만드는 자리

Object Server마다 하나이고, Actor 생성 요청을 승인하거나 거절하며 Actor가 들어오고 나가는
lifecycle을 처리한다.

```cpp
--8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/tictactoe_entry_spot.hpp:doc-entry-spot"
```

### 3.1 Entry Spot이 담지 않는 것

**Entry Spot에는 Actor별 상태를 두지 않는다.** Actor의 상태는 Actor가 소유하고, Entry Spot은
handler와 membership callback만 제공한다. Entry Spot은 Object Server마다 하나뿐이므로 여기에
Actor마다의 값을 쌓으면 그 Object Server가 다루는 Actor 수만큼 자란다.

### 3.2 Actor를 없애는 자리

**Actor는 Entry Spot에서만 없앨 수 있다.** User Spot에 있는 Actor는 먼저 Entry Spot으로
돌아와야 한다 — [Actor membership](35-actor-membership.ko.md)이 그 이동을 다룬다.

없애는 호출은 Entry Spot의 context가 제공하며, 지금 instance를 넘긴다. 이 호출은 membership
callback을 다시 실행하지 않고 Framework의 등록 기록과 묶인 session 경로를 정리한다.

## 4. User Spot — application이 만드는 자리

stable type을 지정해 만들며, 돌아오는 id가 그 뒤 모든 호출의 주소다. 생성 callback에서 거절하면
호출이 실패하고 Spot은 남지 않는다.

```cpp
--8<-- "framework/languages/cpp/samples/TicTacToe/Server/Api/Handlers/create_game_http_handler.hpp:doc-create"
```

## 5. 주입한 서비스의 수명

Framework는 Spot을 활성화할 때 주입 scope를 하나 만들고, Spot 본체와 Spot handler의 의존성을 그
scope에서 만든다. Scope는 Spot이 닫히거나 다른 node로 옮겨 갈 때 함께 정리된다. 따라서
**scoped로 등록한 서비스는 그 Spot이 살아 있는 동안 instance 하나다.** HTTP 요청마다 새로
만들어지는 것과 다르다.

Actor handler는 별도의 Actor 활성화 scope를 사용한다. 서로 다른 Actor는 handler도 scoped 의존성도
공유하지 않는다. Actor가 떠나거나 사라지거나 옮겨 가면 출발 쪽 scope를 정리하고 도착 쪽에서 다시
만든다.

**ORM context를 Spot이나 Spot handler의 생성자로 주입하면 문제가 생긴다.** User Spot 하나가 몇 시간 유지되면 그 context도 같은 기간 유지된다.

| 증상 | 내용 |
| --- | --- |
| 메모리 증가 | change tracker가 조회한 entity를 계속 추적한다 |
| 오래된 값 조회 | 같은 키를 다시 조회해도 추적 중인 이전 instance를 돌려준다 |
| 오류 상태 고착 | 저장 실패로 context가 오염되면 Spot 수명 내내 복구되지 않는다 |

handler type을 transient나 singleton으로 등록해도 Framework가 정한 수명은 바뀌지 않는다.
Framework가 handler를 만들고 의존성만 활성화 scope에서 만들기 때문이다.

**저장소에 Spot에서 직접 접근하지 않는 편을 먼저 고른다.** 저장과 조회는 channel handler가 있는
서비스에 요청하고, Spot은 메모리 상태와 실행 순서만 소유한다. Channel handler는 dispatch마다
scope를 가지므로 ORM을 생성자로 받아도 된다 —
[Channel 메시징](20-channel-messaging.ko.md)의 handler가 그 자리다.

## 6. 관련 문서

- id로 호출하는 상태 객체 — [Spot](21-spot.ko.md) · [Actor](22-actor.ko.md)
- 무엇이 함께 실행되는가 — [실행 모델](32-execution-model.ko.md)
- Actor가 Spot 사이를 옮기는 규칙 — [Actor membership](35-actor-membership.ko.md)
- 실행 위치를 옮기는 것 — [Relocation](37-relocation.ko.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
