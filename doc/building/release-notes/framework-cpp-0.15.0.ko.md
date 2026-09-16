[English](./framework-cpp-0.15.0.md) | [한국어](./framework-cpp-0.15.0.ko.md)

# ZLink C++ Framework 0.15.0 릴리스 노트

Framework 0.15.0는 binding 1.2.0과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 주요 변경

- Classic fanout publisher에 `no_drop` 설정을 더했습니다. 켜면 topic이 일치하는 pipe 전체에 대해 전부 보내거나 하나도 보내지 않으며, 보낼 수 없으면 `DeadlineExceeded`로 끝납니다.
- Fanout subscriber가 받을 topic을 startup에 등록합니다. 등록한 topic의 byte prefix 합집합을 받으며, 등록이 없으면 빈 topic만 받습니다.
- Fanout publish가 local admission을 기다린 뒤 결과를 돌려줍니다. 기다린 시간이 send timeout을 넘으면 `DeadlineExceeded`입니다.
- Descriptor를 처음 받아들이는 자리에서 owner의 생존을 판단합니다. Owner lease가 끝난 descriptor는 admission 단계에서 걸러집니다.
- Wildcard bind host에서 `advertise_host`를 생략하면 같은 address family의 loopback을 광고합니다. `0.0.0.0`은 `127.0.0.1`, `::`은 `::1`입니다. 다른 host에서 접속해야 할 때만 지정하면 됩니다.
- 실행 중 channel weight를 바꾸면 peer에 descriptor update를 보냅니다. 담당 node가 하나뿐인 channel에서 weight를 `0`으로 바꿔도 호출이 계속 성공하던 문제를 고쳤습니다.
- `app::run`이 startup 검증 실패를 보고합니다.

## 설치

[`framework-cpp/v0.15.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.15.0)에서 `zlink-framework-cpp-0.15.0.tar.gz`를 내려받고 `find_package(zlink_framework CONFIG REQUIRED)`를 사용합니다. vcpkg overlay port와 Conan recipe로도 설치할 수 있습니다.
