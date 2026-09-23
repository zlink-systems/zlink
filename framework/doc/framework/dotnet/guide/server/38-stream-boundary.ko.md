---
title: "STREAM의 동작 원리 · C#/.NET"
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
다른 언어로 보기 — [C++](../../../cpp/guide/server/38-stream-boundary.ko.md) · **C#/.NET** · [Java](../../../java/guide/server/38-stream-boundary.ko.md) · [Kotlin](../../../kotlin/guide/server/38-stream-boundary.ko.md) · [Node/TypeScript](../../../node/guide/server/38-stream-boundary.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    연결 하나에서 packet이 session callback에 닿기까지의 순서, 처리가 밀릴 때 일어나는 일,
    응답이 요청과 짝지어지는 방식, 오류와 연결 종료가 어디로 가는지 알 수 있다.

[STREAM](23-stream.ko.md)은 연결을 받아 packet 하나에 답하기까지를 다뤘다. 이 장은 그 사이에서
Framework가 하는 일을 순서대로 설명한다.

## 1. 연결 하나가 지나는 순서

<iframe class="zlink-diagram" src="/common/diagrams/38-stream-dispatch.html" title="연결 하나가 지나는 자리" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/38-stream-dispatch.html" target="_blank">↗ 크게 보기</a></p>

client가 연결하면 Framework가 그 연결의 session을 만들고 연결 callback을 실행한다. 그 뒤 packet
하나는 다음 순서로 처리된다.

1. Framework가 host의 처리 queue에 자리를 하나 확보한다.
2. 자리를 확보한 뒤에만 Core에서 packet 한 건을 꺼낸다.
3. header를 풀어 packet 이름, metadata, 요청 정보와 **packet의 상대 Actor**를 dispatch context에
   담는다. 상대 Actor는 client가 Actor handle로 보냈고 그 slot이 현재 binding일 때만 있다 —
   [Session 묶음의 동작 원리](39-session-binding.ko.md)가 다룬다.
4. session callback에 dispatch context와 아직 변환하지 않은 payload를 넘긴다. payload는
   session callback 안에서 공통 decode 표면으로 원하는 타입으로 읽는다.

연결이 끊기면 연결 해제 callback을 실행한다. 연결을 구분하는 값은 Core가 연결마다 붙인
routing ID이며, session callback까지 그대로 전달된다.

## 2. 처리가 밀릴 때 — 순서와 backpressure

### 2.1 client → 서버

**처리 queue가 가득 찬 동안 완성된 packet은 버리지 않고 Core에 남겨 두며, 처리가 재개되면 연결별
순서대로 한 번씩 session callback에 전달한다.** framing이 잘못되거나 완성되지 않은 packet은 callback에
넣지 않고 연결을 닫는다.

처리 queue에 자리가 없으면 Framework는 다음 packet을 꺼내지 않는다. 꺼내지 않은 packet은 Core의
수신 buffer에 남고, buffer가 한도(HWM)에 이르면 Core가 그 연결의 socket에서 더 읽지 않는다. 그러면
TCP 흐름 제어로 client 쪽 socket의 송신 buffer가 차고 client의 송신이 늦어진다. 자리가 나면 멈췄던
packet이 순서대로 다시 흐른다. queue 자리는 host 전체가 함께 쓰므로 한 연결이 아니라 host의 모든
연결에 같이 적용된다. 한도와 상태 전이는 [Backpressure](33-backpressure.ko.md)가 다룬다.

### 2.2 서버 → client

서버가 session으로 보내는 응답과 push는 **연결마다** Core 송신 buffer에 쌓인다. 그 연결로 실제로
내보내는 속도보다 보내는 속도가 빨라 buffer가 한도(HWM)에 이르면 그 연결로 보내는 send가 자리가 날
때까지 기다린다. 다른 연결로 보내는 send는 영향을 받지 않는다. 기다리다 deadline이 지나면 send는
`DeadlineExceeded`로 끝나며, Framework는 같은 내용을 다시 보내지 않는다.

client connector는 실행 환경의 socket을 그대로 쓰고 받은 것을 한도 없이 계속 읽으므로, client
application의 처리가 느린 것은 서버의 송신을 막지 않는다. push(`Send`)는 client 쪽의 한도 없는 수신
queue에 쌓이고, 응답과 오류 응답은 그 queue를 거치지 않고 기다리던 요청을 완료한다. 서버의 송신이 기다리는 것은 네트워크가 느리거나 client가 socket을 읽지 못할 때다. 받는
쪽 크기 상한(§4)은 이 방향에 적용하지 않는다.

## 3. 응답 token의 수명

요청에 답할 때는 지금 dispatch의 **한 번만 사용하는 응답 token**을 사용한다. 그 token은 지금
요청에서만 유효하고 한 번 제출할 수 있다. timeout이나 취소로 전송이 실패해도 같은 token을 다시
사용할 수 없다.

응답은 요청의 sequence를 그대로 싣고 돌아가며, client는 이 sequence로 기다리던 요청을 찾는다. 응답에는
packet 이름이 실리지 않는다.
응답을 어떤 타입으로 읽을지는 client가 요청할 때 지정한 타입이 정한다. 오류 응답도 같은
sequence로 돌아간다.

기다리는 요청이 없는 client에게 서버가 먼저 보낼 때는 응답이 아니라 보내기(push)를 사용한다.

## 4. 받는 message의 크기 상한

client가 보내는 message 하나(6-byte 길이 prefix를 뺀 header와 payload 합)의 상한은 기본 64 KiB다. 서버가 보내는
message에는 적용하지 않는다. 상한을 넘은 message는 session callback에 일부도 전달하지 않고,
서버에 `EMSGSIZE`를 기록한 뒤 연결을 끊는다. client는 오류 코드를 받지 않고 연결 종료만 본다.
`0`으로 설정하면 Framework 상한을 두지 않는다.

## 5. 오류가 가는 곳

**session 오류 callback은 그 session에 귀속되는 transport 오류만 받는다.** 나머지는 다음
경로로 간다.

| 오류 | 어디로 가나 |
| --- | --- |
| 그 session의 transport 오류 | session 오류 callback |
| handshake 실패 | runtime 관측. session이 만들어지기 전이라 부를 대상이 없다 |
| socket·node 단위 오류 | runtime 관측. session 하나의 오류로 확정할 수 없다 |
| application handler 예외 | handler 예외 처리 경로 |

## 6. 연결을 닫을 때

연결을 닫기 시작하면 Framework는 그 연결에서 새 packet을 받지 않고, 진행 중인 읽기와 쓰기를
끝내거나 취소한 뒤 TCP·TLS·WebSocket 자원을 정리한다. 정리 뒤에 늦게 도착한 transport 완료는
이미 정리된 자원을 쓰지 않는다. 그 연결에 묶였던 Actor가 어떻게 되는지는
[Session 묶음의 동작 원리](39-session-binding.ko.md#3-연결이-끊길-때의-통지)가 다룬다.

## 7. 시작 전에 막는 설정

stream node 등록은 node 이름, bind endpoint와 session type으로 정한다. 다음 설정 오류는 첫
연결까지 미루지 않고 **host가 시작하기 전에** 막는다.

| 조건 |
| --- |
| node 이름이 비어 있다 |
| 같은 node 이름을 두 번 등록했다 |
| bind endpoint가 없다 |
| 같은 session type을 한 host의 둘 이상의 node에 등록했다 |
| 한 node에 session을 둘 이상 등록했다 |
| TLS를 켰는데 인증서 경로가 비어 있다 |
| TLS를 켰는데 key 경로가 비어 있다 |
| TLS server를 설정하지 않고 client 인증서를 요구했다 |
| message 크기 상한이 음수다 |

TLS를 켜면 인증서와 key 경로를 함께 지정한다. client 인증서 요구는 기본이 꺼짐이고, 켜면
검증에 실패한 연결은 **session을 만들기 전에** 거부한다.

## 8. 관련 문서

- 연결을 받아 답하기까지 — [STREAM](23-stream.ko.md)
- 연결 하나를 Actor에 묶기 — [Session과 Actor 연결](24-actor-session.ko.md)
- 묶음의 규칙과 Actor별 packet 구분 — [Session 묶음의 동작 원리](39-session-binding.ko.md)
- 처리 queue와 한도 — [Backpressure](33-backpressure.ko.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
