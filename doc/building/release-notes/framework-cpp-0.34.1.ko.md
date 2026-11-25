[English](./framework-cpp-0.34.1.md) | [한국어](./framework-cpp-0.34.1.ko.md)

# ZLink C++ Framework 0.34.1 릴리스 노트

Framework 0.34.1은 0.34.0의 결함 두 개를 고치는 hotfix입니다. C++ binding 1.26.0과 Core 1.26.0을 그대로 사용합니다.

## 결함 수정

- Spot timer를 같은 이름으로 다시 등록하면 duplicate로 거부됐습니다. 이제 새 timer가 시작된 뒤 같은 이름의 이전 timer를 은퇴시키고, 이전 timer에 남은 tick은 실행하지 않습니다(Spot timer §1). (#5465, #5476)
- WebSocket(`ws://`)·TLS(`tls://`) STREAM listener의 공개 상태가 `tcp://` 주소를 보고했습니다. 이제 설정한 scheme과 실제 주소·port를 보고합니다. (#5460, #5471)

## 설치

플랫폼에 맞는 Framework archive를 [GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.34.1)에서 선택하십시오.

릴리스 태그는 [`framework-cpp/v0.34.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.34.1)입니다.
