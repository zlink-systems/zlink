# progress S-1 (activate_read 왕복 축소)

- 01:14 worktree ~/project/zlink-work/s1 생성(main 430abce139), S-2 diff(`git diff HEAD`) --3way 적용 완료.
- 01:15~01:40 S-A/S-B 근거 절 + pipe.cpp/socket_base_api.cpp 정독.
- 판정 (a): **불필요**. `flush_unlocked`의 `sleeping = !_out_pipe->flush()`는 ypipe의 기존 sleep/awake 표시 그 자체이며,
  reader가 다시 잠들기 전까지 두 번째 flush는 true를 돌려주므로 activate_read는 sleep episode당 정확히 1회다.
  즉 "미소비 activate_read 중복"은 구조상 발생하지 않는다 → 새 pending 플래그는 순수 추가 상태이며 이득 0.
- 구현 (b): `pipe_t::process_activate_read` 핫 경로에서 `_out_sync` 제거(reclassify 콜드 분기에만 유지).
- 구현 (c): `socket_base_t::read_activated`의 `_transport_pair_id!=0` 3회 검사 → pair_id 1회 로드 + lane 1회 로드.
- 01:42 dev 빌드 시작.
- 02:00 dev 빌드 OK. ctest 대상 suite 5회 전부 통과. lost-wake 계열 10회 반복 2차례 전부 통과.
- 02:10 TSan(build-tsan) 4개 바이너리 실행 — 경합은 전부 pre-existing(S-2 base에서도 동일 보고) 확인.
- 02:15 callgrind S-2 base vs S-1: Ir/msg 10,589 -> 9,887 (-6.6%), mutex lock 18.48 -> 16.55 /msg.
- 02:27 with_stream(load 0.45): zlink 287.9/258.9/32.5, asio 363.9/327.2/40.3.
- 02:35 보고서 core-rf-S-1-summary.md 작성 완료. 커밋하지 않음(감독관 리뷰 대기).
- 02:5x 감독관 요청으로 `git checkout --detach 45eaddeb32`(S-2·S-9 포함). 충돌 없이 S-2 훅이 소멸,
  diff는 S-1 전용 2파일(pipe.cpp, socket_base_api.cpp)만 남음. 공개 API/vers diff 없음.
  dev 재빌드 OK, 대상 suite 59/59 통과, lost-wake 6종 --repeat until-fail:5 통과.
