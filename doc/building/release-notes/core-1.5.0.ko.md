[English](./core-1.5.0.md) | [한국어](./core-1.5.0.ko.md)

# libzlink 1.5.0 릴리스 노트

공개 C API와 ABI는 1.4.0과 같습니다(`LIBZLINK_ABI_SOVERSION=0`). 이번 릴리스는
host가 `dlopen()`으로 `libzlink`를 읽을 때 필요한 static TLS 크기를 줄입니다.

## 주요 변경

Beast WebSocket 보안 PRNG는 112바이트 `thread_local` 객체 대신 소유 포인터가
가리키는 thread별 heap 객체를 사용합니다. 따라서 static TLS 여유 공간이 적은
host에서도 `libzlink`를 읽을 수 있습니다. 보고된 Godot 4.4.1 .NET GDExtension
구성도 이 변경의 대상입니다([#1041](https://github.com/zlink-systems/zlink/issues/1041)).

hotpath benchmark는 공유 라이브러리에 연결하며, `ZLINK_BUILD_TESTS`는 test
target에만 정의합니다.

## 소비자에게 미치는 영향

기존 Core 소비자는 소스나 ABI를 변경할 필요가 없습니다. `dlopen()`으로
`libzlink`를 늦게 읽는 application은 host의 static TLS 여유 공간이 적어도
라이브러리를 사용할 수 있습니다.

## 검증

hotpath gate는 공유 라이브러리를 대상으로 benchmark를 실행합니다. test 전용
정의는 test target에만 적용됩니다.

릴리스 태그는 [`core/v1.5.0`](https://github.com/zlink-systems/zlink/releases/tag/core%2Fv1.5.0)입니다.
