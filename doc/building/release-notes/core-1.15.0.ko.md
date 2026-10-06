[English](./core-1.15.0.md) | [한국어](./core-1.15.0.ko.md)

# libzlink 1.15.0 릴리스 노트

## 변경

- `DONTWAIT` SEND·REQUEST 대기 토큰에 기한이 없어졌습니다. 토큰은 자원 회복(`ZLINK_SEND_ADMITTED`), 명시적 대상 제거(`ZLINK_SEND_NOT_FOUND`, STREAM `ZLINK_SEND_NOT_CONNECTED`), socket close·context 종료로만 끝납니다. `SNDTIMEO`는 blocking `NONE` 제출에만 적용합니다. Core는 `ZLINK_SEND_TIMED_OUT`(803)을 더 이상 발행하지 않으며, 값은 ABI 보존을 위해 남깁니다. 1.11.0에서 넣은 대기 토큰 기한을 되돌린 변경입니다 (#1452).
- CMake 컴파일러 플래그 검사가 올바른 결과 변수 이름을 써서 `-Wall`, `-Wextra`, `-pedantic`(MSVC `/W4`, `/WX`)와 `LIBZLINK_WERROR`가 실제로 적용됩니다. 이 플래그가 보고한 경고는 소스에서 고쳤습니다 (#1420).
- Linux 빌드가 `SO_BINDTODEVICE`를 감지합니다 (#1420). 정정: 1.15.0은 `ZLINK_OPT_BINDTODEVICE`를 socket에 적용하지 않고 값을 저장만 합니다. 수정은 #1469입니다.
- 공개 옵션으로 켤 수 없던 내부 socket priority 경로와 쓰이지 않는 `O_CLOEXEC`·TIPC 빌드 검사를 지웠습니다 (#1420).
