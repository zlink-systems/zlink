# progress S-2 (비재귀 mutex)
- 22:03 worktree ~/project/zlink-work/s2 생성(detached 8b65c9b42c). 규칙·브리프·S-B 읽음.
- 22:03-22:10 잠금 전수 조사(정적). 스크립트로 _out_sync 67개 임계구역의 self-call/외부 콜백 추출.
- 22:10 편집 완료: mutex.hpp 통합(mutex_t=plain, recursive_mutex_t=파생), fast_mutex.hpp 삭제,
  증명 안 된 잠금 20개 선언을 recursive_mutex_t로, 디버그/새니타이저 빌드는 ERRORCHECK.
- 22:10-22:16 MACHINE_FREE 대기(6분).
- 22:16 dev 빌드 시작.
- 22:32 dev 빌드 성공. ctest -R 'stream|pipe|mailbox|poller|wake|socket|mutex' 5회 = 45/45 전부 통과.
- 22:42 TSan 트리 구성(LTO OFF, gcc -fsanitize=thread) 후 빌드 시작. ERRORCHECK가 __SANITIZE_THREAD__로 자동 무장됨.
- 다음: TSan 1회 + release --lib-only + with_stream(flock PERF_LOCK).
- 23:05 release --lib-only 성공. 23:08 with_stream 측정 완료(flock, loadavg 3.13/7.38/9.19).
- 23:10 TSan ctest: ASLR 때문에 setarch -R 필요. 45개 중 16개가 TSan 경고로 실패(본체 테스트는 PASS),
  전부 data race(ypipe 알려진 오탐 계열). double lock/EDEADLK/abort 0건 = 재진입 없음 실측 확인.
- 23:20 보고서 doc/plan/c016-worklog/core-rf-S-2-summary.md 완료. 커밋하지 않음. 종료.
