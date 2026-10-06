[English](./core-1.16.0.md) | [한국어](./core-1.16.0.ko.md)

# libzlink 1.16.0 릴리스 노트

## 변경

- Auto HWM의 runtime memory hint option 20을 제거했습니다. snapshot field는 유지하며 항상 0을 반환합니다. Core는 현재 cgroup과 모든 상위 cgroup에서 memory limit을 감지합니다. 새 연결은 수동 Core budget을 설정한 경우에도 resolved memory limit을 기준으로 수용하며, budget은 계속 HWM 계획에 사용합니다 (#1476).
- 연결이 끊기면 monitor ready count에서 해당 연결을 지웁니다. poller가 기다리는 동안 monitor를 닫으면 context 종료가 아닌 socket close를 보고합니다 (#1476, #1083).
- `ZLINK_OPT_BINDTODEVICE`를 `tcp`, `tls`, `ws`, `wss` socket에 적용합니다. listener는 bind 전에, connecter는 connect 전에 적용합니다. 이 옵션은 1.10.0 이전부터 값을 저장만 하고 적용하지 않았습니다. 인터페이스 바인딩을 지원하지 않는 플랫폼에서 비어 있지 않은 값을 설정하면 `ZLINK_CONFIG_NOT_SUPPORTED`를 반환합니다 (#1469).
- 프로세스가 fd 한도에 도달해 socket mailbox의 signaler를 만들 수 없으면 `zlink_socket()`이 보존된 errno(예: `EMFILE`)와 함께 `NULL`을 반환합니다. 이전에는 socket을 반환한 뒤 첫 `zlink_connect()`에서 `Bad file descriptor` 오류로 중단됐습니다 (#1481).
