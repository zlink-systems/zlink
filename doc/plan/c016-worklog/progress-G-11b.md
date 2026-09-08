# G-11b 진행

- 2026-09-07: 공통 규칙 확인, `origin/main` 기반 detached worktree `~/project/zlink-work/g11b` 생성. G-11a가 HEAD 조상임을 확인.
- 2026-09-07: 필독 설계·스펙·원칙 문서 전체 확인 및 hotpath/teardown 동시 접근 증명 완료.
- 2026-09-07: pristine dev 기준 stream_tcp `pthread_mutex_lock` 23.919 calls/msg, TSan 기존 경고 20건 확보.
- 2026-09-07: session I/O writer의 `write`/`flush`만 `_out_sync` 생략, peer credit 원자화와 `_out_active` CAS 적용. 증분 빌드 통과.
- 2026-09-07: suite 5회 중 기존 간헐 1회(단독 3/3 PASS), lost-wake 100/100, close/release 50/50 PASS.
- 2026-09-07: TSan에서 pristine 20건 대비 패치 41건. 신규 21건은 monitor의 `get_msgs_written`/`get_bytes_written`과 session writer ledger 갱신 race로 확인.
- 현재: 부록 A (a)의 접근 집합 전제가 반증되어 runtime 패치를 전부 원복하고 중단 보고서 작성 완료.
