[English](./bindings-cpp-1.16.0.md) | [한국어](./bindings-cpp-1.16.0.ko.md)

# ZLink C++ binding 1.16.0 릴리스 노트

Core 1.16.0을 따릅니다.

## 변경

- Core 1.16.0은 `ZLINK_OPT_BINDTODEVICE`를 `tcp`, `tls`, `ws`, `wss` socket의 bind·connect 전에 적용합니다. 지원하지 않는 플랫폼에서 비어 있지 않은 값을 설정하면 `ZLINK_CONFIG_NOT_SUPPORTED`를 반환합니다 (#1469).
- Core 1.16.0은 socket mailbox signaler를 만들지 못하면(프로세스 fd 한도 도달 포함) `zlink_socket()`에서 `NULL`과 시스템 errno를 반환합니다 (#1481).
- Core Auto HWM의 runtime memory hint를 제거했습니다. binding snapshot에서도 이 hint를 표시하지 않습니다. Core는 상위 cgroup의 limit도 감지하며, 수동 HWM budget과 별개로 resolved memory limit을 기준으로 연결을 수용합니다. 연결 해제 시 monitor ready count도 갱신하며, poller가 기다리는 동안 monitor를 닫으면 socket close를 보고합니다 (#1476, #1083).
