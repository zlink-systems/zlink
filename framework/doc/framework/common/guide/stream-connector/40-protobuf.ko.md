# Node Protobuf 송수신

!!! info "이 장을 읽고 나면"

    `.proto`에서 TypeScript용 클래스를 생성하고, Protobuf push와 요청 응답을 받을 수 있다.
    코드는 `framework/languages/node/tutorial/StreamClient`의 실행 예제에서 읽는다.

서버가 Protobuf payload를 쓰는 경우 client도 같은 `.proto` 정의로 bytes를 읽어야 한다.
이 예제는 `Ping` push와 `Pong` 응답을 받는다. 생성할 때 타입 하나를 지정하는 codec을 사용하며,
여러 수신 타입의 자동 선택 범위는 [Protobuf codec과 타입](41-protobuf-codecs.ko.md)이 설명한다.

## 1. 메시지 코드 생성

`.proto`는 메시지의 field 번호와 타입을 정의하는 파일이다. 이 예제는 문자열을 보내는 `Ping`과
숫자로 응답하는 `Pong`을 정의한다.

```protobuf title="StreamClient/messages.proto"
--8<-- "framework/languages/node/tutorial/StreamClient/messages.proto:protobuf-schema"
```

`framework/languages/node/tutorial/StreamClient`에서 의존성을 설치하고 생성 명령을 실행한다.
`protobufjs-cli`의 static-module은 메시지마다 실행 시점에 존재하는 생성자를 만든다.

```bash
npm ci
npm run generate:protobuf
```

명령은 `pbjs -t static-module -w commonjs`로 `generated/messages.cjs`를 만들고,
`pbts`로 `generated/messages.d.cts`를 만든다. `.cjs` 확장자는 이 ESM project에서 생성 파일을
CommonJS로 읽게 한다. 이 파일들은 빌드할 때 다시 생성한다.

## 2. codec 등록

생성 메시지와 `@zlink-systems/framework-codec-protobuf`의 browser 진입점을 가져온다.
server serializer용 `./framework` 진입점은 client에서 사용하지 않는다.

```typescript title="StreamClient/protobuf.ts"
--8<-- "framework/languages/node/tutorial/StreamClient/protobuf.ts:protobuf-imports"
```

connector를 만들 때 codec 하나를 지정한다. 이 codec은 보내는 payload와 받는 payload를 모두
`Ping`으로 처리한다. `endpoint`는 Protobuf를 사용하는 서버의 WebSocket 주소다.

```typescript title="StreamClient/protobuf.ts"
--8<-- "framework/languages/node/tutorial/StreamClient/protobuf.ts:protobuf-register"
```

## 3. 받는 쪽 — push handler

아래 예제는 동일한 `Ping` push에 세 형태의 handler를 등록한다. 실제 application에서는
필요한 형태 하나를 사용한다. 이름만 지정한 마지막 handler도 이 고정 타입 codec으로는 `Ping`을 받는다.

<iframe class="zlink-diagram" src="/common/diagrams/stream-protobuf-push.html" title="Protobuf push 디코딩" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/stream-protobuf-push.html" target="_blank">↗ 크게 보기</a></p>

```typescript title="StreamClient/protobuf.ts"
--8<-- "framework/languages/node/tutorial/StreamClient/protobuf.ts:typed-receive"
```

서버의 packet 이름이 `Ping`이므로 `Ping` 생성자를 받는 등록과 이름을 직접 받는 등록이 같은
push를 받는다. 생성 클래스 이름과 wire 이름이 다르면 명시 이름 형태를 사용한다.

## 4. 보내는 쪽 — send와 request

앞 절의 handler가 받을 메시지를 보내려면 생성 클래스의 instance를 사용한다. connector는
그 생성자에서 packet 이름 `Ping`을 정한다.

```typescript title="StreamClient/protobuf.ts"
--8<-- "framework/languages/node/tutorial/StreamClient/protobuf.ts:protobuf-send"
```

`Pong`으로 오는 응답에는 별도의 디코딩 타입이 필요하다. 응답 bytes를 받는 `submitEncoded()`를
호출한 뒤, `fromProto()`에 응답의 생성 클래스를 전달한다.

<iframe class="zlink-diagram" src="/common/diagrams/stream-protobuf-reply.html" title="Protobuf 요청 응답" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/stream-protobuf-reply.html" target="_blank">↗ 크게 보기</a></p>

```typescript title="StreamClient/protobuf.ts"
--8<-- "framework/languages/node/tutorial/StreamClient/protobuf.ts:protobuf-request"
```

`submit<Pong>()`의 `<Pong>`은 TypeScript 반환 타입만 지정한다. 실행 시점에는 `Pong` 생성자가
codec으로 전달되지 않으므로, 위 예제처럼 응답 타입을 지정해야 한다.

## 5. 실행 결과

이 명령은 StreamClient의 검증용 WebSocket peer를 임시 로컬 포트에 실행하고 같은 tutorial 함수를
호출한다. 별도로 Server를 실행할 필요가 없다. peer는 `Ping`을 push로 돌려주고, request에는
`Pong { rank: 7 }`으로 응답한다.

```bash
cd framework/languages/node/tutorial/StreamClient
npm ci
npm run protobuf:check
# protobuf: push=hello, reply.rank=7
```

검증은 실제 WebSocket 송수신, 세 수신 등록의 payload, 응답 타입을 확인한다. 잘못된 codec 번호와
손상된 Protobuf bytes가 거부되는지도 확인한다. Node는 이 browser connector의 검증 환경으로 사용한다.

## 6. 관련 문서

- 타입·이름 선택과 여러 메시지의 제약 — [Protobuf codec과 타입](41-protobuf-codecs.ko.md)
- 수신 타입 전달과 handler 실행 시점 — [packet 수신](05-receiving.ko.md)
- send와 request — [packet 송신](04-sending.ko.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
