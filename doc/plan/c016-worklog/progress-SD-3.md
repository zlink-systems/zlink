# core-rf-SD-3 진행

- 2026-09-08 23:26 KST: 공통 규칙, SD-2 보고서 §2·§3.2·§6, 현재 diff와 B2-only patch를 확인했다. `SUPERVISOR-NOTE.md`는 범위 밖 사용자 파일로 보존한다.
- 현재 단계: B1 readiness rearm/TCP speculative 금지 변경을 제거하고 B2의 1-hit 성장·`last_read_bytes` 단일 상태와 WS message-boundary gather만 남기는 patch를 구성한다.
- 2026-09-08 23:32 KST: B1 transport/interface·readiness predicate를 제거하고 app+B2+D-f+WS gate 구성으로 정리했다. dev build 성공, `unittest_asio_write_turn_policy` 단독 성공, 관련 27-suite 3회 27/27 성공(15.05/14.83/14.64 s).
- 현재 단계: 전체 dev ctest(`-E hotpath_gate`)를 실행한다.
- 2026-09-08 23:37 KST: 전체 dev ctest(`-E hotpath_gate`) 211/211 성공(244.44 s).
- 현재 단계: 기존 `core/build-tsan`을 현재 source로 갱신하고 관련 27-suite를 실행한다.
- 2026-09-08 23:44 KST: 기존 GCC TSan 트리를 현재 source로 갱신하고 `TSAN_OPTIONS=halt_on_error=1`(suppression 미지정)로 관련 27-suite 27/27 성공(22.80 s), TSan 보고 0건. 축소 strace 셀은 64 KiB/CCU20/I/O1, load1=0.22에서 성공했다.
- strace: message 11,380, `recvfrom` 22,889회/오류 11,387회로 성공 read 11,502회=1.01072/msg, EAGAIN 1.00062/msg. B2-only 특성과 일치한다.
- 현재 단계: 현재 source의 Release library를 갱신한 뒤 idle WS/WSS gate 1회를 실행한다.
- 2026-09-08 23:49 KST: Release library 갱신 성공. load1 1.58인 사전 실행은 첫 TCP 셀 중 중단하고 invalid-load로 분리했다. load1 0.74, ninja 0에서 시작한 유효 1회는 12/12 complete, ws Q64/Q1=2.099380, wss=1.300809로 PASS했다.
- 2026-09-08 23:53 KST: 보고서와 base `84d25131a6` 기준 `all-artifacts/SD-3.patch` 생성 완료. hotpath reference와 `SUPERVISOR-NOTE.md`는 제외했고 reverse apply check 및 `git diff --check`를 통과했다. SHA-256 `9a16a25fa1b0005a0a99124f5751f798a2469548d1c25d53c956c783cd160084`.
