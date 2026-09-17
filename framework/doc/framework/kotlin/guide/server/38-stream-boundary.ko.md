---
title: "STREAM의 동작 원리 · Kotlin"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/38-stream-boundary.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# STREAM의 동작 원리

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: Handler와 메시지 처리](31-handler-dispatch.ko.md) | [다음: Session 묶음의 동작 원리](39-session-binding.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C#/.NET](../../../dotnet/guide/server/38-stream-boundary.ko.md) · [C++](../../../cpp/guide/server/38-stream-boundary.ko.md) · [Java](../../../java/guide/server/38-stream-boundary.ko.md) · **Kotlin** · [Node/TypeScript](../../../node/guide/server/38-stream-boundary.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    stream node 등록이 언제 거부되는지, 오류가 어디로 가는지, 응답 하나가 어떻게
    짝지어지는지 알 수 있다.

[STREAM](23-stream.ko.md)은 연결을 받아 packet 하나에 답하기까지를 다뤘다. 이 장은 그 경계에서
Framework가 정해 둔 것을 다룬다 — 시작할 때 막는 조건, 오류의 귀속, 응답 token의 수명, 그리고
접속하는 쪽이 고르는 실행 방식이다.

## 1. 등록을 시작 전에 막는 조건

등록은 node 이름과 bind endpoint와 session type으로 정한다. **그중 bind endpoint는 반드시
지정한다.** 선언에 표시를 달아 stream node가 자동으로 등록되는 표면은 없다.

다음 조건은 첫 연결까지 미루지 않고 **host가 시작하기 전에** 설정 오류로 막는다.

| 조건 |
| --- |
| node 이름이 비어 있다 |
| 같은 node 이름을 두 번 등록했다 |
| bind endpoint가 없다 |
| 같은 session type을 중복 등록했다 |
| 한 node에 session을 둘 이상 등록했다 |
| TLS를 켰는데 인증서 경로가 비어 있다 |
| TLS를 켰는데 key 경로가 비어 있다 |
| TLS server를 설정하지 않고 client 인증서를 요구했다 |

TLS를 켜면 인증서와 key 경로를 함께 지정한다. client 인증서 요구는 기본이 꺼짐이고, 켜면
검증에 실패한 연결은 **session을 만들기 전에** 거부한다.

## 2. 오류가 가는 곳

<iframe class="zlink-diagram" src="/common/diagrams/38-stream-dispatch.html" title="연결 하나가 지나는 자리" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/38-stream-dispatch.html" target="_blank">↗ 크게 보기</a></p>

**session 오류 callback은 그 session에 귀속되는 transport 오류만 받는다.** 나머지는 다음
경로로 간다.

| 오류 | 어디로 가나 |
| --- | --- |
| 그 session의 transport 오류 | session 오류 callback |
| handshake 실패 | runtime 관측. session이 만들어지기 전이라 부를 대상이 없다 |
| socket·node 단위 오류 | runtime 관측. session 하나의 오류로 확정할 수 없다 |
| application handler 예외 | handler 예외 처리 경로. **session 오류 callback이 아니다** |

**handler filter는 session dispatch에 적용되지 않는다.** 다른 dispatch에 등록한 filter가 있어도
session callback 앞에서는 실행되지 않는다 —
[Handler와 메시지 처리](31-handler-dispatch.ko.md#2-filter--공통-처리를-한곳에-모은다)가 filter의
범위를 다룬다. 인증처럼 session 경로에서 걸러야 하는 일은 session의 handler 등록으로 처리한다.

**받기 loop를 직접 도는 표면은 없다.** Framework가 packet을 queue에 넣은 뒤 session callback을
실행하며, 그 경계에서 dispatch·주입·기록을 일관되게 적용한다. loop·취소·backpressure를
application이 직접 구현하지 않게 하려는 설계다 — [Backpressure](33-backpressure.ko.md)가 그 뒤를 다룬다.

## 3. 응답 token의 수명

요청에 답할 때는 지금 dispatch의 **한 번만 사용하는 응답 token**을 사용한다. 그 token은 지금 요청에서만
유효하고 한 번 제출할 수 있다. timeout이나 취소로 전송이 실패해도 같은 token을 다시 사용할 수 없다.

**응답에는 packet 이름이 실리지 않는다.** 접속한 쪽은 요청 sequence만으로 기다리던 요청을 찾고,
응답을 어떤 타입으로 읽을지는 **호출할 때 지정한 타입**이 정한다. 이름으로 고르지 않으므로 응답
쪽에 packet 이름을 붙이는 표면도 없다. 오류 응답도 같은 sequence로 돌아온다.

기다리는 요청이 없는 상대에게 서버가 먼저 보낼 때는 응답이 아니라 보내기를 사용한다.

## 4. 접속하는 쪽이 고르는 실행 방식

즉시 방식은 connector의 worker에서 callback을 실행하므로 실행 위치를 가리지 않는 client에
맞는다. game loop나 UI thread처럼 실행 위치가 정해진 client는 수동 방식을 골라, application이
호출하는 그 시점에 자기 loop 안에서 밀린 것을 처리한다.

### 4.1 관측 비용을 끄는 값

connector는 server runtime과 같은 진단 수준 옵션을 받는다. 기본값은 오류만 남기는
단계이고, 가장 낮은 단계로 낮추면 connector가 나가는 frame에 흐름 식별자를 만들거나 붙이지
않아 관측 전용 비용이 사라진다.

요청과 응답을 짝짓는 데 사용하는 값은 진단이 아니라 protocol 정보이므로 가장 낮은 단계에서도 그대로
동작한다.

## 5. 관련 문서

- 연결을 받아 답하기까지 — [STREAM](23-stream.ko.md)
- 연결 하나를 Actor에 묶기 — [Session과 Actor 연결](24-actor-session.ko.md)
- 묶음의 규칙 — [Session 묶음의 동작 원리](39-session-binding.ko.md)
- filter가 적용되는 범위 — [Handler와 메시지 처리](31-handler-dispatch.ko.md)
- client가 설치할 package — [설치](../../../install.ko.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
