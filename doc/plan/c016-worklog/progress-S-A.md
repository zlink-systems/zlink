# S-A 진행 (측정 전용)

- 22:00 브리프·S-B 보고서 읽음. **perf 없음**(WSL2, perf 바이너리 부재 + `perf_event_open` EACCES/미지원 — 확인 프로그램 `/tmp/pe.c` 로 hw/sw 이벤트 둘 다 fd=-1). strace/ltrace/gdb 도 없음.
  → 대체 계측: (1) `/proc/<pid>/io`(syscr/syscw/rchar/wchar) + `/proc/<pid>/task/*/status`(ctxsw) + `task/*/stat`(스레드별 CPU) 실시간 샘플링, (2) 명령 수·심볼 분해는 valgrind callgrind(축소 CCU).
- 22:10 6개 셀(zlink/asio × 64/1024/65536 B, CCU 1000, io 4) 실행 완료, 베이스라인과 일치.
- 22:20 /proc 샘플 분석 완료. 22:25 callgrind(CCU 20) zlink/asio 병렬 실행 중.
- 보고서: doc/plan/c016-worklog/core-rf-S-A-stream-profile.md, 원본: scratchpad/S-A/
- 22:35 callgrind(CCU 20, 1024 B) zlink/asio 완료 + 기동분 차감 측정. 보고서 작성 완료.
- 22:40 **완료**. 결론: 메시지당 CPU 1.52×, 명령 수 2.22×(축소셀), syscall 5.99 vs 2.11/msg, eventfd wake 1.03/msg, ctxsw 2×, futex 0(비경합).
