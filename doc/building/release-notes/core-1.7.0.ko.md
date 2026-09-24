[English](./core-1.7.0.md) | [한국어](./core-1.7.0.ko.md)

# libzlink 1.7.0 릴리스 노트

공개 C API와 ABI는 1.6.0과 같습니다(`LIBZLINK_ABI_SOVERSION=0`).

## 주요 변경

`libzlink`가 더 이상 `initial-exec` 모델의 libstdc++ TLS를 참조하지 않습니다.
Core의 mutex와 condition variable을 사용한 완료 처리가 `std::promise`를
대체합니다. Linux에서 Godot C++ GDExtension을 처음 editor에 가져올 때
static TLS 부족으로 라이브러리 로드에 실패하던 문제를 해결합니다
([#1041](https://github.com/zlink-systems/zlink/issues/1041)).

## 소비자에게 미치는 영향

기존 Core 소비자는 소스나 ABI를 변경할 필요가 없습니다. Linux의 Godot editor는
첫 C++ GDExtension 가져오기에서 `libzlink`를 로드할 수 있습니다.

## 검증

CTest가 `libzlink`에 `initial-exec` 모델의 libstdc++ TLS 참조가 없는지 확인합니다.

릴리스 태그는 [`core/v1.7.0`](https://github.com/zlink-systems/zlink/releases/tag/core%2Fv1.7.0)입니다.
