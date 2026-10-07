---
title: "Node Protobuf codec과 타입 · Node/TypeScript"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/stream-connector/41-protobuf-codecs.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# Node Protobuf codec과 타입

<!-- framework-adapter-nav:start -->
[목차](README.ko.md) | [이전: Node Protobuf 송수신](40-protobuf.ko.md)
<!-- framework-adapter-nav:end -->

!!! info "이 장을 읽고 나면"

    생성 클래스 codec과 envelope codec의 차이를 알고, packet 이름과 디코딩 타입을 구분할 수 있다.
    실행 코드는 [Protobuf 송수신](40-protobuf.ko.md)의 StreamClient tutorial과 같다.

Protobuf bytes만으로는 어떤 메시지 클래스인지 알 수 없다. field 번호와 값은 들어 있지만,
`Ping`이나 `Pong`이라는 클래스 이름은 들어 있지 않기 때문이다. packet 이름으로 handler를 고르는
동작과, bytes를 어떤 클래스로 읽을지 정하는 동작은 별개다.

## 1. handler 타입과 기본 타입

`createZlinkStreamProtobufCodec(Type)`은 handler에 지정한 생성자의 `decode`를 호출한다.
`on(Ping, handler)`와 `on(Pong, handler)`는 같은 codec으로 서로 다른 타입을 받는다.
타입 없이 이름만 등록하면 factory에 지정한 기본 `type`으로 디코딩한다.
송신은 생성 instance의 `encode`를 사용한다. 생성자의 `encode`가 없는 값은 기본 타입으로 인코딩한다.

자체 envelope 형식을 이미 쓰는 protocol에는 `createZlinkStreamProtobufEnvelopeCodec(options)`가 있다.
envelope는 여러 메시지를 구분할 정보를 포함하는 바깥 메시지 형식이다. factory는
`options.decode(payload, messageType)`에 handler 타입을 전달한다. 내부 메시지를 읽는 방법은
application의 decoder가 정한다. 메시지 종류를 자동 등록하는 기능은 제공하지 않는다.

| 표면                                              | 타입을 정하는 곳                    | 수신 동작                                       |
| ------------------------------------------------- | ----------------------------------- | ----------------------------------------------- |
| `createZlinkStreamProtobufCodec(Type)`            | handler 등록, 없으면 factory 기본값 | 지정한 생성자의 `decode` 호출                   |
| `createZlinkStreamProtobufEnvelopeCodec(options)` | handler 등록과 application decoder  | `options.decode(payload, messageType)`에 위임   |
| `fromProto(payload, ReplyType)`                   | 호출할 때                           | 지정한 `ReplyType`으로 encoded payload를 읽는다 |

!!! note "수정 버전"

    위 타입 전달은 [#1503](https://github.com/zlink-systems/zlink/issues/1503)의 수정 동작이다.
    배포된 0.28.0의 두 factory는 handler 타입을 사용하지 않는다.
    `submit(Pong)`과 `submitCallback`도 이 수정의 추가 API다.

## 2. 서버 packet 이름과 생성 클래스

packet 이름은 Protobuf schema의 field가 아니라 connector가 보내는 별도의 이름이다.
타입 기반 송신과 수신은 기본 name resolver가 생성자의 `name`에서 이름을 정한다.
예제의 `.proto` package는 `tutorial`이지만 packet 이름은 `tutorial.Ping`이 아니라 `Ping`이다.

서버 handler가 `Ping`을 받고 push 이름도 `Ping`이면 예제의 이름이 그대로 일치한다.
서버 이름이 `player.ping`이면 송신 builder의 `packetName('player.ping')`과
`on('player.ping', handler, Ping)`으로 그 이름을 지정한다.
생성자 이름을 줄이는 bundle 설정에서도 wire 이름을 명시하거나 생성자 이름을 보존해야 한다.

## 3. 요청 응답의 타입

`request(new Ping(...))`의 생성자는 요청을 인코딩하고 요청 packet 이름을 정하는 데 사용된다.
응답 타입은 `submit(Pong)`에 전달한 생성 클래스가 정한다. request 빌더는
기존 codec의 `decode(payload, Pong)`으로 응답을 읽는다. callback 형태인
`submitCallback(Pong, callback)`도 같은 디코딩 경로를 사용한다.
타입을 지정하지 않은 `submit()`과 `submit(signal)`은 기존 codec의 기본 디코딩을 사용하고,
`submitCallback(callback)`은 encoded payload를 전달한다.
기존 `submit(callback)`은 `submitCallback(callback)`으로 이름이 바뀌었다.
저수준 대안은 `submitEncoded()`와 `fromProto(encodedReply, Pong)`이다.

<iframe class="zlink-diagram" src="/common/diagrams/stream-protobuf-reply.html" title="명시적 응답 타입 디코딩" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/stream-protobuf-reply.html" target="_blank">↗ 크게 보기</a></p>

```typescript title="StreamClient/protobuf.ts"
--8<-- "framework/languages/node/tutorial/StreamClient/protobuf.ts:protobuf-request"
```

응답은 요청의 sequence와 연결되므로 `Pong` packet 이름만 보고 디코딩 타입을 자동으로 찾지 않는다.
서버와 client는 응답에도 같은 `.proto` field 번호와 타입을 사용해야 한다. 틀린 schema로 읽었을 때
항상 오류가 나는 것은 아니다. 알려지지 않은 field가 무시되어 기본값이 반환될 수도 있다.

## 4. 관련 문서

- 코드 생성부터 실행 확인까지 — [Protobuf 송수신](40-protobuf.ko.md)
- handler 타입 전달 — [packet 수신](05-receiving.ko.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
