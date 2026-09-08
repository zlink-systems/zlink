# core-rf-SD-1 진행

- 2026-09-08 18:40 KST: 작업 시작. `wip/sd-1`, clean worktree 확인. 공통 규칙 확인 완료; 입력 문서와 코드 근거 재검증 중.
- 2026-09-08 18:46 KST: 입력 스펙/계획과 POSDDD 대조 완료. base `84d25131a6424031149ab7321e0678b65409bd75` Release+LTO lib 빌드·보관 완료(`SD-1-base-lib`, SHA256 `1ed8fd9c...`). 계측 app 수정 착수.
- 2026-09-08 18:48 KST: 단계 1 완료. zlink 계측의 chunk-local 다중 frame parser를 제거하고, exact 1-frame zero-copy 외 모든 byte를 per-RID `frame_buffer_t`로 조립하도록 통일. B2 target 규칙 수정 중.
- 2026-09-08 18:54 KST: 단계 2 완료. decoder/encoder의 `full_hits` 상태와 required=2 규칙을 제거; full request/batch 1회면 2배 성장(max clamp), encoder 기존 short-batch 축소 유지.
- 2026-09-08 18:55 KST: 단계 3 완료. async/speculative read 결과를 기존 pipeline 상태 하나로 통합하고, normal completion/restart 모두 `should_speculatively_read_stream()`의 full-read evidence 및 동일 bounded drain을 사용하도록 변경.
- 2026-09-08 18:55 KST: 단계 4 완료. STREAM gather env 접근자 3개 제거. 실제 RAW 연결은 `protocol_builds_gather_header=false`로 항상 차단됨을 생성자 경로에서 확인; ZMP용 `ZLINK_ASIO_GATHER_WRITE`만 유지.
- 2026-09-08 19:02 KST: dev 빌드(JOBS=4, RelWithDebInfo/LTO off) 성공. 관련 unittest와 stream/asio/decoder/ws suite 실행 준비.
- 2026-09-08 19:10 KST: 관련 stream/asio/decoder/ws 27-test suite 3/3회 성공, 전체 `ctest -E hotpath_gate` 211/211 성공(233.44 s). TSan 구성 확인 중.
- 2026-09-08 19:13 KST: GCC TSan/LTO off 빌드 후 `setarch x86_64 -R`, suppression 없이 관련 27-test suite 27/27 성공; ThreadSanitizer 보고 0건. Release+LTO/hotpath 및 A/B 측정 준비 중.
- 2026-09-08 19:20 KST: idle(load1 0.74, ninja 0, available 10727 MiB)과 `/tmp/claude-1000/PERF_LOCK` 확보 후 base 4-stack × 3-size × 3-run 측정 시작. base Core SHA와 보관본 일치 확인.
- 2026-09-08 19:33 KST: base 36셀 측정 완료(모두 mismatch 0). 동일 S-D 조건(CCU20, I/O 1, warmup 2 s + 5 s) strace는 `recvfrom` 24,272/8,044 msg = 3.017/msg(오류 1.000/msg). base WS 64 KiB 대조 셀 준비 중.
- 2026-09-08 19:46 KST: base WS gate용 12셀 1회 완료(도구 판정 FAIL: ws Q64/Q1=0.430835, wss=1.781459). patch Release+LTO clean JOBS=4 빌드와 hotpath_bench JOBS=4 빌드 완료; idle 대기 후 hotpath/patch A/B 실행 예정.
- 2026-09-08 19:50 KST: hotpath 5셀은 4 PASS/1 FAIL(DR Ir ratio 0.9492; 단일 재측정 0.9497로 재현, STREAM 1.0027 PASS). idle(load1 0.51, ninja 0, available 10703 MiB)에서 patch 36셀 측정 시작.
- 2026-09-08 20:06 KST: patch 36셀 모두 mismatch 0. strace는 `recvfrom` 23,408/11,644 msg=2.010/msg(오류 1.000/msg): 성공 read는 2.017→1.010으로 줄었지만 B1의 EAGAIN 제거 목표는 미달. patch WS gate 12셀은 FAIL(wss Q64/Q1=0.799415). D-S1 실험 착수.
- 2026-09-08 20:22 KST: D-S1 `rcvbuf=-1` 실험 3-size×3-run 완료. main 대비 처리량 +6.3~+16.7%, CCU1000 RSS 170,796→169,692 KiB; CCU4000은 기존 10 s CLIENT_READY 경계에서 양쪽 모두 실패하여 timeout 우회 없이 무효 처리. 실험 코드는 본 worktree에서 제거하고 별도 patch로 보관.
- 2026-09-08 20:30 KST: 본 patch/실험 patch 재생성 및 적용성 확인, 최종 Release+LTO와 focused dev test 재검증 완료. 보고서 작성 및 산출물 해시 확인 중.
- 2026-09-08 20:45 KST: 보고서 독립 2축 검토 완료. 원시 결과와 대조해 D-S1 p95 표기, combined patch 인과 한계, D-f 스펙 동시 반영 조건, 규칙 수와 base ref 이동을 교정함. 최종 산출물 완료.
