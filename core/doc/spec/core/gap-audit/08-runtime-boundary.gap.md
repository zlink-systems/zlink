# Runtime Boundary 스펙–구현 gap 감사

> 감사 도구: codex (정적 코드 대조, 실행 테스트 미실행) · 2026-08-24
> 대조 범위: `core/include/`, `core/src/`, `core/CMakeLists.txt`, `core/src/libzlink.vers`

판정: **D. 구현 서술 낡음 1건**. 공개 경계의 구현 gap(B), 문서-코드 모순(C),
문서 누락(A)은 발견하지 못했다. 스펙·코드는 수정하지 않았고, 이 보고서만 작성했다.

## 대조 완료 계약군

- 설치 공개 header와 export inventory: service header·service C ABI·ChannelName/Spot/Actor API 없음, root `zlink.h`도 raw header만 포함 — 일치
- Core 제공 raw 표면: Context, message, PAIR·PUB/SUB·DEALER/ROUTER·STREAM, endpoint, TCP·WebSocket·TLS, monitor, poller, timer, thread, stopwatch, atomic counter, proxy — 일치
- transport liveness 경계: transport 실패 monitor/reconnect 구현과 TCP keepalive·재전송 상한 option 경로 존재, heartbeat option·command·engine state 없음 — 일치
- paired DEALER/ROUTER: application/completion 두 lane, pair ID·generation·peer identity 검증, 한 lane 종료 시 반대 lane 종료 — 일치
- receive-flow 경계: 설정 API 1개, monitor event 3개와 status snapshot field, internal frame encode/decode·completion-lane 소비 및 application receive 차단 — 일치
- raw-only source 경계: service protocol/state machine, mailbox·claim, Spot·Actor, Location/Checkpoint Store와 service maintenance 의미 없음 — 일치
- internal payload/terminal 경계: reply payload는 completion pipe에서 callback으로 직접 전달되고, completion queue에는 payload 없는 terminal control만 보관 — 일치

## Gap 목록

| 분류 | 스펙 근거 | 코드 근거 | 판단 |
|---|---|---|---|
| D. 구현 서술 낡음 | `08-runtime-boundary.ko.md:124` — "Core 0.9.0"의 내부 계층 설명 | `core/include/zlink.h:8-14`, `core/CMakeLists.txt:11`, `core/src/api/core/context_api.cpp:79-83` — 공개 ABI와 `zlink_version()`은 `0.13.0`을 보고한다. | 이 절은 내부 구조의 현재 사실을 설명한다고 명시한다(`08-runtime-boundary.ko.md:119-122`). 따라서 서술의 버전 `0.9.0`은 현 구현의 공개 버전과 다르며 문서 갱신 대상이다. raw-only 경계 설명 자체는 코드와 일치한다. |

## 요확인

- `08-runtime-boundary.ko.md:228-229`의 **설치 tree와 실제 exported symbol** 검사는 CMake 공개 header 목록과 `core/src/libzlink.vers`로 정적 대조했다. install 산출물과 최종 shared library의 symbol table을 직접 확인하려면 별도 build/install 검증이 필요하다. 이 감사 지침에 따라 실행하지 않았다.
- `08-runtime-boundary.ko.md:160-162,202-203`의 close 이후 late engine callback 및 timer callback의 owner-state 재접근 차단은 여러 비동기 종료 경로의 runtime interleaving까지 포함한다. source의 lifecycle gate·timer cancel 경로만으로는 모든 경쟁 순서를 확정할 수 없으므로, 이 보고서의 gap 표에는 넣지 않았다.
