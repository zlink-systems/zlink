# posddd-rf1 progress

- 2026-09-03: 작업 시작. 현재 브랜치 `perf/phase2-judge`, 작업 트리 clean 확인. 담당 범위와 금지 사항 확인.
- 2026-09-03: 필수 문서 `posddd.ko.md` 전체와 Core module structure/source layout/hot-path 계약을 읽음. 최근 6개 커밋 및 HEAD stat, 최근 Core 커밋 `f3be895b3f`·`8b6c2aa906`의 담당 경로 변경을 대조 시작.
- 2026-09-03: 담당 경로를 part/message, request-reply, completion queue/API 묶음으로 나눠 읽기 전용 병렬 감사 시작.
- 2026-09-03: 1차 감사 완료. production 호출자 0인 DEALER typed-receive 경로와 전용 token/checkout, bulk request-reply send, private accessor·wrapper 후보를 전 트리 `rg`로 확인. 범위 밖 test/runtime 호출이 남은 구 경로는 삭제 보류 대상으로 분리.
- 2026-09-03: 불필요 코드·pass-through 1차 반영. 도달 불가 DEALER typed-receive 책임, 호출자 0 함수/필드, frame close·socket type·send-scope 위임, request-part 중복 검사를 제거하고 request-reply close 상태 전이를 한 함수로 통합.
- 2026-09-03: part-helper receive-ready 캐시 게시를 `publish_buffered_recv_readiness` 한 곳으로 모으고 reset 시 active 전이를 먼저 게시하도록 정리. socket pin 해제와 상태 mutex 순서는 유지.
- 2026-09-03: completion queue의 hot-path `has_ready()` mutex 조회를 enqueue/dequeue/close 전이에서 게시하는 atomic readiness 캐시로 교체. outstanding intrusive list 명명 통일, 중복 reset 제거, payload release 책임 공유.
- 2026-09-03: request-timeout scheduler의 write-only `completed`/deadline 필드와 중복 iterator 검사를 제거하고 deadline 정렬 자료구조 명명을 정리. socket API의 동일 반환 분기와 단일 호출 socket-type wrapper 제거.
- 2026-09-03: 중간 `ulimit -v 16777216 && cmake --build core/build -j4` 성공(243/243). 이후 책임 분리·명명 정리를 계속 진행.
- 2026-09-03: send sequence를 begin/resume fast path로, rollback을 공통 함수로 분리. ROUTER receive staging과 pending endpoint/RID 실패 종료 루프를 각각 단일 책임 함수로 통합하고 receive transport pair 게시를 staging mutex 트랜잭션 안으로 이동.
- 2026-09-03: DEALER dead typed-receive 경로와 전용 reply-target/token 계열, 호출자 0 helper·field·const accessor를 삭제. ROUTER reply-target, completion outstanding list, timeout deadline index, routing-id 명명을 역할에 맞게 정리.
- 2026-09-03: 최신 편집분 `ulimit -v 16777216 && cmake --build core/build -j4` 성공(107/107). 교차 diff 리뷰에서 신규 ABI·correctness 회귀 없음 확인; readiness true 게시는 empty→non-empty 전이로 한정하고 기존 inline-cache release race는 동작 변경이 필요한 BLOCKER로 분리.
- 2026-09-03: 최종 diff 교차 리뷰에서 begin-send topic 할당 OOM 시 scope 해제보다 cache false 게시가 앞설 수 있는 경합을 발견해 `send_scope.reset()`→marker false 순서로 보정. request/reply 예약·게시·rollback·close·timeout 및 나머지 part/message·queue 변경은 동작·ABI 보존 확인.
- 2026-09-03: 최종 `ulimit -v 16777216 && cmake --build core/build -j4` 성공(86/86). 관련 part-helper/completion/timeout/request-reply 테스트 15개 전부 통과.
- 2026-09-03: 전체 `ctest --test-dir core/build -j2 --output-on-failure` 134/134 통과(164.64초). 알려진 `test_single_lane_flow_snapshot_accounting`도 첫 실행 통과. `git diff --check` 및 raw header mirror 12개 비교 모두 green.
- 2026-09-03: 최종 변경 범위 25개 파일이 모두 `core/src/api/socket/**` 안임을 확인. +607/-1478, 순감 871줄. 항목별 근거·파일 묶음·BLOCKERS를 `posddd-rf1-summary.md`에 기록.
