# PUBSUB/inproc 64 KiB 회귀 수정 진행

- 2026-09-04 시작: `main`=`8d58b7f891d34affd49b42f52930895e2d9a2684`, 작업 트리 clean 확인.
- 범위/완료 조건: 두 Core 커밋 편집 A/B로 원인 확정, A의 ledger/alias 계약을 보존한 근본 수정, 지정 gate와 candidate/baseline 재측정. 금지 경로와 git 작업은 건드리지 않음.
- 지정 문서와 커밋 diff 정독: 64 KiB payload charge는 `65536 + sizeof(msg_t)`이고, message path의 registry map/mutex 및 무조건 wake는 금지. a339149dbb의 peer-control commit/release 대칭과 e3d5c5b79f의 connect별 alias/reconnect 보존은 유지해야 함.
- 초기 순차 측정(현재 빌드): candidate 106,373.6 msg/s, baseline 109,371.2 msg/s, ratio 0.973. 동시 측정값(238,219/232,047)은 자원 경쟁으로 무효 처리.
- 편집 A/B(각 `libzlink`만 재빌드, runs=3 median): a339149dbb 단독 되돌림 127,221.8 msg/s; e3d5c5b79f 단독 되돌림 106,610.0 msg/s. 두 임시 되돌림은 diff 0으로 원복 확인. alias 커밋은 영향 없음; pipe.cpp 커밋 경계에서 처리량 차이가 관찰되지만 해당 분기는 PUB/SUB에서 도달 불가라 코드 배치/캐시 또는 변동 여부를 추가 분리함.
- 교차 A/B 2차: current 84,847.6 → a339149dbb revert 103,407.6 msg/s(+21.9%); 다시 current 복원 86,066.8. baseline 인접 측정은 100,122.8/115,953.6으로 자체 변동이 큼. 두 번 모두 a339 revert가 약 20% 회복해, semantic branch 실행이 아니라 compiler가 rare ledger body를 매 flush에 inline한 code-footprint 효과로 경계를 좁힘.
- 수정 실험: `append_pending_peer_controls_unlocked()`를 GCC/Clang `cold,noinline`, MSVC `noinline`로 격리. ELF에서 `flush_unlocked.part.0`이 0x78a→0x4c6이고 peer-control body 0x344가 cold symbol로 분리됨. 동일 직전 current 대비 102,197.0 msg/s(+18.7%)로 revert 이득을 보존하면서 ledger commit을 유지.
- 결정적 보존 테스트 추가: session-backed Completion FLOWSTATE를 enqueue하면 registry completion current/pending이 정확히 증가하고 dequeue 후 0인지 검증. fixed 경로 PASS; a339 로직 임시 제거 시 새 case에서 target timeout으로 FAIL 확인 후 즉시 원복.
- 인라이닝 격리 단독 최종 판정: noinline-only candidate/baseline 108,465.8/125,317.6=0.866으로 gate 실패. 실행되지 않는 ledger body의 배치 변화는 회귀 촉발 요인이지만 근본 수정이 아니므로 attribute 실험을 전부 제거함.
- 근본 원인 확인: PUB record charge는 65,733B라 1MiB HWM에 15개만 들어가고 약 8개 drain마다 LWM wake 경계를 지난다. XPUB NODROP의 message preflight가 HWM에서 `_out_active=false`를 arm한 뒤에는, reader가 peer atomic에 credit를 이미 publish했어도 owner mailbox의 `activate_write` 처리 전까지 재시도마다 즉시 HWM_FULL을 반환했다. 1,024B 셀보다 이 scheduler 경계를 약 54배 자주 지나 64 KiB만 크게 증폭된다.
- 최소 수정: 유일한 호출자인 `dist_t`가 matching/active 소유권을 유지하는 message-aware preflight에서만 `_out_active=false`일 때 peer published credit를 재확인하고 waiter를 clear한다. 일반 `check_hwm()`/LB probe는 그대로라 passive wake invariant를 보존한다.
- 새 결정적 wake 테스트와 기존 passive invariant test를 함께 포함한 `test_router_multiple_dealers` PASS. owner wake 명령 처리 전 dist 재시도가 READY/전송/수신되고, 뒤늦은 명령은 중복 activation을 만들지 않음을 검증한다.
- 성능 원인성: noinline+wake candidate/baseline 123,718.4/111,224.8=1.112. attribute를 제거한 wake-only 최종 후보도 120,769.4/105,994.6=1.139 PASS (raw candidate 126.13/120.77/83.82 Kmsg/s, baseline 99.49/105.99/143.88 Kmsg/s).
- 새 wake 테스트의 구동부를 임시 구동작으로 되돌리면 `test_dist_message_preflight_consumes_published_credit_before_owner_wake`가 즉시 FAIL(`Expected 0 Was 1`)하고 기존 passive LB test는 PASS함을 확인한 뒤 원복. ledger 보존 테스트도 assertion을 pipe/socket cleanup 뒤로 옮겨 회귀 시 timeout 없이 결과를 남기도록 정리함.
- 최종 gate: full build PASS; 전체 ctest 139/139 PASS; 별도 `hotpath_gate` 1/1 PASS; `wake-invariant` 3/3 PASS; raw header mirror cmp 12/12 PASS; `git diff --check` PASS.
- 최종 post-gate 재측정: candidate raw 134.06/145.08/103.87 Kmsg/s, median 134,057.4; baseline raw 126.96/129.63/118.30 Kmsg/s, median 126,964.0; ratio 1.056 PASS. 최종 diff는 Core `pipe.cpp`와 integration test 한 파일뿐이며 branch/HEAD 유지.
