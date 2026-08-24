# Connection Memory 스펙-구현 gap 감사

> 감사 도구: codex (정적 코드 대조, 실행 테스트 미실행) · 2026-08-24
> 범위: `core/doc/spec/core/systems/05-connection-memory.ko.md`와 `core/include/`, `core/src/`, 관련 `core/tests/` 표본

판정: **구현/문서 gap 2건, 요확인 0건**. 코드와 스펙 문서는 수정하지 않았으며, 이 보고서만 작성했다.

## 대조 완료 계약군

- 문서가 Auto HWM에 위임한 byte 회계·HWM admission·retained-credit lease·oversize 상세 계약은 중복 계상하지 않았다.
- frame charge의 payload + `sizeof(zlink_msg_t)` 계산, decoder reservation, multipart provisional/committed 전환과 반환 경로: 위임 문서의 감사 범위로 제외
- application directional HWM과 retained receive의 byte 보존, exact origin generation credit 반환과 retired origin 유지: 위임 문서의 감사 범위로 제외
- empty-pipe oversize 조건과 Core budget/Auto HWM planning 제외: 위임 문서의 감사 범위로 제외
- context Auto HWM snapshot의 application lease·completion current byte·oversize 누적 관측 필드: 일치
- completion lane의 HWM/LWM 0 적용과 application planning/reservation 제외: 일치
- 일반 전송 connection의 session, engine, session pipe 생성과 TLS 추가 storage: 일치

## Gap 목록

| 분류 | 스펙 근거 | 코드 근거 | 판단 |
|---|---|---|---|
| D. 구현 서술 낡음 | `systems/05-connection-memory.ko.md:18-22`, `37-43` — 각 transport connection의 고정 구성 요소로 session·engine·pipe endpoint·handshake buffer·운영체제 socket 구조를 열거 | `core/src/runtime/sockets/common/socket_base_endpoint.cpp:168-220`, `core/src/runtime/core/pipe.cpp:87-105` | `inproc`도 connection-ready를 만들지만 session/engine 없이 `pipepair`만 생성하며, 코드도 “inproc routes have no engine”이라고 명시한다. 따라서 열거한 구성 요소는 socket 기반 transport에는 적용되어도 모든 transport connection의 고정 구성 요소라는 구현 서술은 과도하다. inproc와 socket 기반 transport를 구분해야 한다. |
| C. 문서-코드 모순 | `systems/05-connection-memory.ko.md:86-90` — completion progress lane은 terminal reply와 error reply 전용 | `core/include/zlink/socket/api.h:372-390`, `core/src/runtime/sockets/common/socket_base_flow_state.cpp:129-161`, `core/tests/integration/test_flow_state_paired.cpp:280-298` | 공개 `zlink_socket_set_receive_flow_state`는 DEALER/ROUTER completion lane으로 RUNNING/PAUSED flow-state frame을 동기화한다. 이 frame은 terminal reply나 error reply가 아니며, 통합 test도 실제 completion lane에서 pause/resume을 전달함을 확인한다. “전용” 제약은 현재 구현과 모순된다. |

## 요확인

- 없음.
