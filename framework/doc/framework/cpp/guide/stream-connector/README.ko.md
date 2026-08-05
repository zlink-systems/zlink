# C++ Stream Connector

> **⚠️ 이 가이드는 최신이 아니다.** 현재 리뷰·정비가 끝난 가이드는
> [`.NET` 가이드](../../../dotnet/README.ko.md) 하나뿐이다. 이 문서는 그 이전 상태이며,
> **`.NET` 가이드가 완성되면 이 문서를 삭제하고 그것을 기준으로 다시 쓴다.**
>
> **계약을 확인할 때는 이 문서를 믿지 말고 [spec 트리](../../../common/spec/README.ko.md)를 본다.**

C++ STREAM client connector 제품군의 문서 진입점이다. **네이티브 빌드 게임 엔진**(Unreal,
Godot GDExtension, Axmol), 일반 C++ 애플리케이션, 서버 e2e/perf client가 대상이다.

| 문서 | 내용 |
|------|------|
| [가이드 INDEX](INDEX.ko.md) | 개요, 시작하기, 옵션, 송수신, lifecycle, 엔진 어댑터, 패키징, 성능 |
| [core — async runtime](core/guide/async-runtime.ko.md) | no-exception·no-coroutine core runtime |
| [e2e-client — coroutine client](e2e-client/guide/coroutine-client.ko.md) | 서버 e2e/perf용 coroutine helper |
| [Stream Connector 공통 스펙](../../../common/spec/stream-connector/32-stream-connector.ko.md) | **정본** — 대상 환경, transport, wire 계약 |

> **웹(WASM) 빌드에는 이 connector를 쓸 수 없다.** Cocos Creator web과 Godot Web은
> [Node.js/TypeScript connector](../../../node/guide/stream-connector/README.ko.md)를 사용한다.
