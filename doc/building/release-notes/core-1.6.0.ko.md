[English](./core-1.6.0.md) | [한국어](./core-1.6.0.ko.md)

# libzlink 1.6.0 릴리스 노트

공개 C API와 ABI는 1.5.0과 같습니다(`LIBZLINK_ABI_SOVERSION=0`).

## 주요 변경

ROUTER가 완전한 REQUEST를 수신한 뒤 source pipe의 연결이 종료되어도
reply token 발행을 `ECONNABORTED`(`INTERNAL_ERROR 206`)로 거부하지
않습니다. 물리적 연결 종료는 reply token을 무효화하지 않습니다
([#1051](https://github.com/zlink-systems/zlink/issues/1051)).

## 소비자에게 미치는 영향

기존 Core 소비자는 소스나 ABI를 변경할 필요가 없습니다. 완전한 REQUEST를
수신한 뒤 source pipe의 연결이 종료되어도 ROUTER는 REQUEST와 reply token을
반환합니다.

## 검증

회귀 test는 완전한 REQUEST를 수신한 뒤 source pipe를 종료하고, ROUTER가
REQUEST와 reply token을 반환하는지 확인합니다.

릴리스 태그는 [`core/v1.6.0`](https://github.com/zlink-systems/zlink/releases/tag/core%2Fv1.6.0)입니다.
