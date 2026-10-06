# Node Protobuf 송수신

!!! info "이 장을 읽고 나면"

    `.proto`에서 TypeScript용 클래스를 생성하고, Protobuf push와 요청 응답을 받을 수 있다.
    코드는 `framework/languages/node/tutorial/StreamClient`의 실행 예제에서 읽는다.

서버가 Protobuf payload를 쓰는 경우 client도 같은 `.proto` 정의로 bytes를 읽어야 한다.
이 예제는 codec 하나로 `Ping`과 `Pong` push를 각각 `on(Type, handler)`에 지정한 타입으로 받는다.
메시지 종류가 255개여도 codec을 255개 만들 필요는 없다. 각 생성 클래스를 handler에 전달한다.
타입 선택과 이름의 관계는 [Protobuf codec과 타입](41-protobuf-codecs.ko.md)이 설명한다.

!!! note "수정 버전"

    이 장은 [#1503](https://github.com/zlink-systems/zlink/issues/1503)의 codec 수정 동작을 설명한다.
    배포된 0.28.0은 handler 타입을 사용하지 않는다. 배포 전 검증에는 수정한 로컬 package를 사용한다.

## 1. 메시지 코드 생성

`.proto`는 메시지의 field 번호와 타입을 정의하는 파일이다. 이 예제는 문자열을 보내는 `Ping`과
숫자를 보내는 `Pong`을 정의한다.

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

connector를 만들 때 codec 하나를 지정한다. 여기서 `Ping`은 수신 타입이 없는 경우의 기본값이다.
송신은 생성 instance의 타입을, 타입 지정 수신은 handler에 전달한 생성자를 사용한다. `endpoint`는 Protobuf를 사용하는 서버의 WebSocket 주소다.

```typescript title="StreamClient/protobuf.ts"
--8<-- "framework/languages/node/tutorial/StreamClient/protobuf.ts:protobuf-register"
```

## 3. 받는 쪽 — push handler

`on(Ping, handler)`는 `Ping`을, `on(Pong, handler)`는 `Pong`을 디코딩 타입으로 전달한다.
같은 codec이 두 생성자의 `decode`를 각각 호출하므로 handler는 서로 다른 타입의 instance를 받는다.
예제는 이름을 직접 지정하는 형태와 타입을 생략해 기본값을 쓰는 형태도 함께 보여 준다.

<iframe class="zlink-diagram" src="/common/diagrams/stream-protobuf-push.html" title="Protobuf push 디코딩" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/stream-protobuf-push.html" target="_blank">↗ 크게 보기</a></p>

```typescript title="StreamClient/protobuf.ts"
--8<-- "framework/languages/node/tutorial/StreamClient/protobuf.ts:typed-receive"
```

서버의 packet 이름이 `Ping`이므로 `Ping` 생성자를 받는 등록과 이름을 직접 받는 등록이 같은
push를 받는다. 생성 클래스 이름과 wire 이름이 다르면 명시 이름 형태를 사용한다.

## 4. 보내는 쪽 — send와 request

앞 절의 handler가 받을 메시지를 보내려면 생성 클래스의 instance를 사용한다. connector는
그 생성자에서 packet 이름을 정하고, codec은 해당 생성자의 `encode`로 bytes를 만든다.

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
호출한다. 별도로 Server를 실행할 필요가 없다. peer는 `Ping`과 `Pong { rank: 3 }`을 push로 보내고, request에는
`Pong { rank: 7 }`으로 응답한다.

```bash
cd framework/languages/node/tutorial/StreamClient
npm ci
# 수정 배포 전: 로컬 codec 소스를 빌드한다.
npx --yes --package=esbuild@0.25.5 esbuild ../../packages/framework-codec-protobuf/src/index.ts \
  --bundle --format=esm --platform=browser --target=es2022 \
  --external:@zlink-systems/stream-wire \
  --outfile=node_modules/@zlink-systems/framework-codec-protobuf/dist/browser/index.mjs
npm run protobuf:check
# protobuf: Ping=hello, Pong.rank=3, reply.rank=7
```

검증은 실제 WebSocket 송수신, 두 메시지 타입의 handler payload와 송신 bytes, 응답 타입을 확인한다. 잘못된 codec 번호와
손상된 Protobuf bytes가 거부되는지도 확인한다. 검증 프로그램은 Node의 WebSocket으로 서버에 연결하며 browser용 connector 진입점을 실행한다.

## 6. 관련 문서

- 타입·이름 선택과 기본 타입 — [Protobuf codec과 타입](41-protobuf-codecs.ko.md)
- 수신 타입 전달과 handler 실행 시점 — [packet 수신](05-receiving.ko.md)
- send와 request — [packet 송신](04-sending.ko.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
