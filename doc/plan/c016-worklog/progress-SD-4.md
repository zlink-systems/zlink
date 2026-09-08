# core-rf-SD-4 진행

- 2026-09-08: 작업 시작. 공통 규칙, branch와 SD-3 누적 diff 경계를 확인했다. 리뷰·스펙·기존 batch/gather 구현 대조 중이다.
- 2026-09-09: 원인을 `prepare_output_buffer()`의 target 경계에서 body가 다음 zero-copy write로 넘어가는 상태기로 좁혔다. 128 KiB target에 들어가는 64 KiB+small은 기존 copy batch, target 초과·128 KiB max 이내 body는 `[batch, body]`, max 초과는 기존 분리로 설계했다.
- 2026-09-09: WS 기본 단일-message gather gate를 제거했다. bounded batch 안에서만 target-crossing body를 두 번째 buffer로 유지하고, 같은 제출 helper와 기존 pipeline 수명 상태를 재사용했다. policy 반례 2건을 추가했다.
- 2026-09-09: dev 빌드 성공. `test_zmp_ws_wss`, `test_asio_ws`, WS/policy unit 4/4와 관련 27-suite 3회(27/27 ×3)가 통과했다. 전체 hotpath 제외 gate를 실행한다.
- 2026-09-09: 전체 dev 211/211, TSan 27/27·report 0건 통과. 첫 성능 iteration은 ws 0.511946 실패(wss 0.887032): 128 KiB target에서 64 KiB body를 copy해 RR 비용이 남았다. 큰 body가 준비된 batch를 항상 닫되 합계 max 이내일 때만 writev하는 규칙으로 좁혔다.
- 2026-09-09: 후속 성능은 ws 1.147/1.147, wss 0.836/0.676. 두 번째 WSS 실패 원인을 zero-copy body까지 encoder copy-target 성장량에 더한 변경으로 좁혔다. 기존 규칙대로 실제 batch buffer byte만 성장 입력으로 복원했다.
- 2026-09-09: 기존 env gather와 isolated WSS 2셀 비교는 13.740 vs bounded 10.324 kops로 분산이 컸다. 기존 trace gate를 잠시 사용해 bounded 후보가 body 65536, batch 0/8, target 4096에서 모두 선택됨을 확인했고 임시 로그를 삭제했다. 코드를 고정하고 최종 검증을 재시작한다.
- 2026-09-09: 고정된 최종 Release 소스로 idle WS/WSS 12셀을 3회 실행했고 ws Q64/Q1=1.171861/1.257310/1.172784, wss=0.931437/1.067634/1.082682로 모두 통과했다. release-gate 빌드 후 hotpath 5셀은 4 PASS, 변경과 무관한 dealer-router inproc 셀 0.9496 한 건이 경계 실패했다. 반복 측정 없이 잔여 실패로 기록하고 보고서·patch를 확정한다.
- 2026-09-09: SD-4 보고서를 작성하고 base `84d25131a6` 누적 patch를 생성했다. patch는 SD-3 report와 SD-4 report를 포함하며 supervisor note·hotpath reference를 제외했고 reverse apply check와 `git diff --check`를 통과했다. SHA-256은 `b94c8dd24b1091d238a5fce0b4644c9cc0faf761a277c5a48fe8688eeed10ed1`다.
