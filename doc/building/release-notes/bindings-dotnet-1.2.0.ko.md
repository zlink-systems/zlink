[English](./bindings-dotnet-1.2.0.md) | [한국어](./bindings-dotnet-1.2.0.ko.md)

# ZLink .NET binding 1.2.0 릴리스 노트

Core 1.2.0 위에서 동작하는 릴리스입니다. 1.1.0 binding은 Core 1.1.0 ABI를 전제하므로 Core 1.2.0과 함께 쓸 수 없고, 이 릴리스가 그 간극을 닫습니다.

## 변경

- Core 1.2.0 native와 결합합니다. 패키지에 담긴 native와 provenance는 Core 1.2.0 릴리스 자산입니다.
- `PublishSubmitOperation.Submit()`이 호출자의 flags를 그대로 전달합니다. 기본은 즉시 시도(DontWait)이고, `.Flags(SendFlags.None)`을 명시하면 `SendTimeout`까지 local admission을 기다립니다(#456).
- `CommonSocketOptions.SendTimeout`이 `SNDTIMEO`에 연결됩니다(#456).
## 검증

- binding 테스트 스위트가 Core 1.2.0 패키지 위에서 통과했습니다.

릴리스 태그는 `dotnet/v1.2.0`입니다.
