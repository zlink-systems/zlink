# ALL-2 진행

- 2026-09-08: 공통 규칙 확인. ALL-1 기준 HEAD `25355fcfc5`, tracked diff와 untracked 8개를 `/tmp/zlink-all2.Vu3NrK`에 보존했다.
- 2026-09-08: `origin/main` `14ec63e2c4`로 이동. 충돌 4개(`socket_base_api.cpp`, `socket_base_lifecycle.cpp`, `socket_base_msg.cpp`, `socket_runtime.hpp`)에서 ALL-1의 단일 lifecycle turn을 채택했고, ALL-1 신규 8개 파일을 복원했다.
- 2026-09-08: D-ALL-1은 `pair.pump()`로 첫 route 종료를 결정적으로 완료한 뒤 두 번째 `disconnect_rid`가 `NOT_FOUND`/`ENOENT`인지 검사하도록 수정했다. runtime 변경 없음.
- 2026-09-08: diff check, 공개 interface diff 0, mirror 12/12를 확인했다. dev build는 `ninja=0`, available 10749MB에서 `JOBS=4`로 성공했다(기존 `unittest_monitor_ready_drain` GCC stringop-overflow 경고 1건 관측).
- 2026-09-08: D-ALL-1 focused 첫 실행에서 단일 `pair.pump()`가 막 enqueue된 command를 놓쳐 기존 OK가 재현됐다. route probe가 `EHOSTUNREACH`를 볼 때까지 bounded pump하도록 보강한 뒤 owners 20/20 통과.
- 2026-09-08: 전체 dev 210/210(249.23초), 관련 정규식 116×2(264.77초), 신규 6×10(32.51초) 통과.
- 2026-09-08: TSan 전수 빌드 성공. 전체 209/210, runtime ThreadSanitizer warning/summary 0. `test_stream_packet_progress/test_shutdown_during_drain`은 기존과 같은 262K fragment 3초 적재 전제에서 실패했고 단독 재실행도 같은 assertion으로 실패했다.
- 2026-09-08: ASan 신규·변경 13 target 13/13 통과(12.42초). TSan fixture 실패는 기대값/timeout을 완화하지 않고 남은 실패로 보고한다.
- 2026-09-08: 최종 `git diff --check`, 공개 interface diff 0, mirror 12/12 재확인. 신규 7개에 intent-to-add 후 누적 patch 생성(507,426 bytes, SHA-256 `0514d34c567b7533a33f4d498bf708c7a68328e358625fda809d69d6af981b14`).
- 완료: ALL-2 보고서와 남은 TSan fixture 실패를 기록했다. 커밋·stash·스펙 수정·성능 실행 없음.
