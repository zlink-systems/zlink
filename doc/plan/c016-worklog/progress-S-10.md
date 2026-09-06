# S-10 진행

- 09:00 worktree ~/project/zlink-work/s10 생성(detached b52c8b1055).
- TLS 사용처 전수 조사 완료(7곳). callgrind 호출자 분해로 메시지당 분포 확인.
- 설계 (A) `-ftls-model=initial-exec` 채택, core/CMakeLists.txt 수정. dlopen 헤드룸 실측(1664 B) vs libzlink PT_TLS 296 B.
- dev 빌드 시작.
- dev 빌드 성공. libzlink.so: GD 재배치 0, TPOFF 15, `__tls_get_addr` 심볼 0, PT_TLS 0x130(304 B).
- python ctypes dlopen OK. ctest -R 'recv|stream|router|dealer|poll' 5회(-j4) → run3에서 test_close_completion_poller_release 1회 실패(다른 job 빌드 부하), 단독 3회·직렬 전체 1회 모두 통과.
- release --lib-only 빌드 시작 → callgrind/with_stream 측정(PERF_LOCK flock).
- 완료: callgrind after 8회/75,664 msg(전부 libstdc++ __cxa_get_globals), 총 Ir/msg 11,096 → 10,253.
- with_stream 2회(flock) 완료. 보고서 core-rf-S-10-summary.md 작성. 커밋 없음.
