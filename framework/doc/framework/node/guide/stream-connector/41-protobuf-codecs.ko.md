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

    고정 타입 codec과 envelope codec의 차이를 알고, packet 이름과 디코딩 타입을 구분할 수 있다.
    실행 코드는 [Protobuf 송수신](40-protobuf.ko.md)의 StreamClient tutorial과 같다.

Protobuf bytes만으로는 어떤 메시지 클래스인지 알 수 없다. field 번호와 값은 들어 있지만,
`Ping`이나 `Pong`이라는 클래스 이름은 들어 있지 않기 때문이다. packet 이름으로 handler를 고르는
동작과, bytes를 어떤 클래스로 읽을지 정하는 동작은 별개다.

## 1. 고정 타입과 envelope

메시지 하나를 처리하는 경우에는 `createZlinkStreamProtobufCodec(Type)`을 사용한다.
이 factory는 생성할 때 받은 `Type.encode`와 `Type.decode`만 호출한다.
handler마다 다른 생성자를 전달해도 그 선택은 바뀌지 않는다.

자체 envelope 형식을 이미 쓰는 protocol에는 `createZlinkStreamProtobufEnvelopeCodec(options)`가 있다.
여기서 envelope는 여러 메시지를 구분할 정보를 포함하는 바깥 메시지 형식이다. factory는
`options.encode`와 `options.decode`를 호출하며, 메시지 종류를 자동으로 등록하는 기능은 제공하지 않는다.

| 표면 | 타입을 정하는 곳 | 현재 수신 동작 |
|---|---|---|
| `createZlinkStreamProtobufCodec(Type)` | factory를 호출할 때 | 모든 payload를 같은 `type`으로 읽는다 |
| `createZlinkStreamProtobufEnvelopeCodec(options)` | application의 envelope 형식 | `options.decode(payload)`에 위임하며 handler 타입은 넘기지 않는다 |
| `fromProto(payload, ReplyType)` | 호출할 때 | 지정한 `ReplyType`으로 encoded payload를 읽는다 |

!!! warning "여러 수신 타입의 자동 선택"

    connector는 `on(Type, handler)` 또는 `on(name, handler, Type)`의 타입을 codec에 전달한다.
    그러나 위 두 Protobuf factory는 그 인자를 사용하지 않는다. 서로 다른 `.proto` 메시지를
    handler 타입만으로 선택하는 사용법은 현재 helper로 제공되지 않는다.
    이 차이는 [#1503](https://github.com/zlink-systems/zlink/issues/1503)에서 확인 중이다.

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
이 생성자가 응답 타입까지 정하지는 않는다. 또한 `submit<Pong>()`의 타입 인자는 실행 시점에
존재하지 않는다. 현재 응답 처리 방식은 `submitEncoded()`와 `fromProto(encodedReply, Pong)`이다.

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
