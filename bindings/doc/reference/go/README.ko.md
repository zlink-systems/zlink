한국어 | [English](README.en.md)

[Go 바인딩 스펙](../../spec/go/README.ko.md) · [Go 바인딩 가이드](../../guide/go/index.ko.md)

# Go 바인딩 레퍼런스

Go 바인딩을 사용할 때는 `zlink.systems/zlink`를 가져온다. socket은 `ctx.DealerSocket()`처럼
`Context` method로 만든다. 역할에 맞는 socket에서 typed option과 operation builder를 사용한다.

`zlink.systems/zlink/contracts` 패키지는 값 타입, 열거형과 오류를 정의한다. 루트 패키지는
이 타입들을 다시 내보내고 Core 자원을 사용하는 핸들을 제공한다.

- 공통 option은 `CommonOptions()`로, PUB·XPUB의 전용 option은 `PubOptions()`로
  접근한다. DEALER·ROUTER·STREAM의 전용 option은 각 socket method로 제공한다.
- request의 `Submit(ctx)`는 `RequestSubmission`을 반환한다. 응답 part나 최종 오류는
  `Reply(ctx)`로 받는다. public poller가 `PollCompletion`을 처리하면 결과를 기다리는 동안
  별도 goroutine에서 그 poller를 구동한다.
- `ZlinkError`는 `Error()`, `Code()`, `InternalErrno()`를 제공하는 interface다.
  각 typed error는 `Unwrap()`도 구현하므로 `errors.Is`와 `errors.As`로 오류를 확인할 수 있다.

이 문서 트리의 작성 기준은 [레퍼런스 작성 가이드](../../../../doc/principal/documentation/reference-writing-guide.ko.md)를 따른다.

## 로케일 관례

`bindings/doc/spec/<lang>/`의 모든 문서는 English 원본, Korean 번역이다(framework의
interface-catalog 관례와 반대). 이 레퍼런스 트리도 같은 방향을 따른다 — `.en.md`를
먼저, `.ko.md`를 나중에 사용하고, 모든 spec 인용은 같은 로케일의 spec 파일을 가리킨다.

## 분류

- [Core](01-core.ko.md)
- [Messaging](02-messaging.ko.md)
- [Sockets](03-sockets.ko.md)
- [Eventing](04-eventing.ko.md)
- [Errors](05-errors.ko.md)
