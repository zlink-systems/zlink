[English](./core-1.16.0.md) | [한국어](./core-1.16.0.ko.md)

# libzlink 1.16.0 릴리스 노트

## 변경

- `ZLINK_OPT_BINDTODEVICE`를 `tcp`, `tls`, `ws`, `wss` socket에 적용합니다. listener는 bind 전에, connecter는 connect 전에 적용합니다. 이 옵션은 1.10.0 이전부터 값을 저장만 하고 적용하지 않았습니다. 인터페이스 바인딩을 지원하지 않는 플랫폼에서 비어 있지 않은 값을 설정하면 `ZLINK_CONFIG_NOT_SUPPORTED`를 반환합니다 (#1469).
- 프로세스가 fd 한도에 도달해 socket mailbox의 signaler를 만들 수 없으면 `zlink_socket()`이 보존된 errno(예: `EMFILE`)와 함께 `NULL`을 반환합니다. 이전에는 socket을 반환한 뒤 첫 `zlink_connect()`에서 `Bad file descriptor` 오류로 중단됐습니다 (#1481).
