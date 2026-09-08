# WS-1 진행

- 2026-09-08: 시작. 공통 규칙과 branch/worktree 상태 확인 완료. `main`의 기존 untracked 파일은 건드리지 않음. 문서·결함·결정 기록 및 계측 경로 조사 중.
- 2026-09-08: detached worktree `/home/hep7hep7/project/zlink-work/ws1` 생성(HEAD `40bc3e4910`). WS 구현이 Boost.Beast `ws_transport_t`와 Asio engine에 있고, `auto_fragment(false)`, 64 KiB write buffer, message당 `async_write`를 쓰는 것을 확인. 로컬 dev 빌드 대기 조건 확인 중.
- 2026-09-08: `pgrep -c -x ninja=0` 확인 뒤 `JOBS=4 scripts/build-core.sh dev` 포그라운드 실행 중. 구성 성공(WebSocket/WSS 활성, RelWithDebInfo, LTO off), 약 56% 진행.
- 2026-09-08: dev Core 및 C perf 4개 target 빌드 완료. 재현(load 2.32/4.21/2.43, 2.51/4.13/2.44): ws/tcp 처리량 비 1024 B 단방향 0.937·왕복 0.884, 65536 B 단방향 0.747·왕복 0.322. Callgrind용 서버/클라이언트 command-prefix 실험 패치 추가(main 미적용).
- 2026-09-08: 4 clients·1 s·각 1 I/O thread 왕복 65536 B callgrind 완료(tcp 489 ops/s, ws 111 ops/s). WS 서버 수신 unmask와 client 송신 mask가 각각 약 32.28M Ir로 프로세스 전체의 48.2%/27.9%; 16 KiB `ws_batch_policy` 때문에 큰 payload 하나가 여러 Beast write/frame으로 나뉘는 증거를 확인. 크기별 계측 진행 중.
- 2026-09-08: 1024/4096/65536 B 양쪽 프로세스 callgrind 완료. 64 KiB strace(4 clients): tcp send/recv 계열 합계 11,352회/1,331 ops, ws 18,465회/823 ops(ops당 8.53 vs 22.44). 기존 gather opt-in은 ws 64 KiB 12.74k→5.36k ops/s로 악화되어 단독 해결책에서 기각. 16 KiB batch 확대 가설 실험 준비.
## 2026-09-08 08:06 KST

- 16 KiB WS encoder batch를 256 KiB로 키운 실험: 왕복 WS 64 KiB 15,799.7 ops/s(+24.0%).
- `ZLINK_WS_WRITE_BUFFER_BYTES=262144`도 함께 적용: 17,477 ops/s(+37.2%). TCP 39,501 ops/s에는 여전히 크게 못 미쳐 frame/write 증폭은 2차 원인으로 판단.
- Beast masking 루프를 4 B에서 8 B 처리로 바꾼 실험은 동시 부하가 높아 wall-clock 결과를 채택하지 않고, 동일 축소 셀 callgrind Ir로 재검증 중.

## 2026-09-08 08:16 KST

- 8 B masking 실험의 16 KiB chunk당 비용이 46.9k→10.2k Ir(-78.2%)로 감소. wall-clock은 load 3.23이라 폐기하고 instruction 결과만 채택.
- 기준 소스로 복원·재빌드 완료. 다른 job의 `ninja -j4`가 계속 실행 중이므로 규칙대로 60 s 간격으로 대기하며 단방향 callgrind를 보류 중.
- 계약 확인: `01-zmp` §8/§9는 WS binary message 경계와 ZMP frame 경계를 분리하고 bounded encoder batch를 Beast write 1회로 제출하도록 함. client masking은 RFC 6455 의무지만 4 B 루프와 16 KiB batch 크기는 구현 선택.

## 2026-09-08 08:33 KST

- ninja 0 구간에 기준 소스 단방향 64 KiB callgrind 완료: TCP 257, WS 230 msg/s(비 0.895).
- 단방향 WS에도 mask가 client 25.9%, server 62.4% 존재. 따라서 비용 유무 차이가 아니라 왕복 dependency에서 frame completion·mask가 critical path가 되는 차이로 확정.
- 분석 보고서 초안 작성 완료. 관련 WS 테스트와 diff/status 최종 점검 중.

## 2026-09-08 08:36 KST

- 관련 기준 테스트 `test_asio_ws`, `test_zmp_ws_wss`, `unittest_ws_transport_config` 3/3 통과.
- 제품 소스는 모두 기준으로 복원됨. worktree diff는 callgrind/strace용 C perf runner prefix만 남고, mask/batch 실험 diff는 보고서에만 첨부.
- 최종 보고서 `core-rf-WS-1-analysis.md` 완료.
