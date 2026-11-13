# #1672 C++ 리뷰 (감독자, 2026-10-15) — 브랜치 전체 origin/main...814ee0a710 (32파일 +1431/-761)
이전 머신 커밋(e9eb53fcf0, a36c91189c, bb26c57e54, c2acf8c58a, 52ee330842)과 m1672c(③②④⑤⑥ + merge)를 브랜치 전체 diff로 줄 단위 리뷰. main 소스·시험 전부 읽음.
판정: 승인 조건부(C-F1·C-F2 수정 뒤). 스펙 gap: C-F1 하나.
- ③ 승인: 회전당 poll 1회(begin_receive_turn) 뒤 receive_next 반복. poll 실패 시 protocol_error도 budget.account로 회전이 유한. accept_application_receive=false면 dequeue 없음(기존 fence 유지).
- ② 승인: request_to_target 층 제거, observe_request 동기화, admission 실패 처리는 main의 await_request_admission과 같은 의미.
- ④ 승인: reply를 zlink::message_t로 completion까지 유지, copy()는 참조 공유. reply payload 검증은 application_payload_hwm_bytes(span)로 구조 검증 유지. ClientServer reply는 to_bytes 1회로 main과 복사 수 같음.
- ⑤ 승인: retained_application_message_t 참조 수 — 실패 경로(part 생성 실패·decode 예외)에서 해제 균형 확인.
- 완료 dispatcher lock-free(bb26c57e54) 승인: Treiber push + consumer 일괄 reverse로 FIFO, CAS 실패 시 관찰한 prefix를 소유하지 않음(ABA 무해), 빈 stack에서만 notify, stopping bit 보존. drain_state atomic wait.
- job queue packed credit(a36c91189c) 승인: lock 없는 release 빠른 경로 — waiter 등록 직후 settle_released_capacity, pause 게시 후 재평가로 두 경합을 닫음. FIFO(waiter 있으면 try_reserve 거절) 유지. 경합 시험 2개(1024회) 있음.
- 시험: m6a의 wait_for_activity 단언 2개 삭제(c2acf8c58a) 승인 — 관리 deadline에서 먼저 깨는 것이 정상이며 mailbox pending==1 관찰로 대체(더 강함). 나머지 시험 변경은 callback 타입 변경뿐, 단언 변경 없음. sleep 없음.
- C-F1 (스펙 gap, 반려): ⑥ classify_node_target이 admitted도 not-required도 아닌 target을 그대로 진행시켜, 연결 안 된 Object Client RID의 send가 not_connected로 끝난다. 02-channel-messaging.ko.md:99-100은 NotFound. main은 매 send마다 Location Store를 훑었다(mesh_node_runtime.cpp classify_node_direct_target). 수정: topology에 있으면 topology로 판정(정상 경로, 조회 없음), 어느 쪽에도 없는 target만 기존 Location Store 판정으로 — 오류 경로에서만 비용. 연결 안 된 Object Client RID send·request → NotFound 시험 추가. 스펙 개정 불필요.
- C-F2: lock-free dispatcher의 state_t 소멸 시 pending에 남은 item이 해제되지 않는다(main은 unique_ptr head 소멸로 item 소멸자가 owner를 놓았다). state_t 소멸자에서 남은 chain을 delete.
- worktree 범위 밖 미추적 파일 framework/languages/engines/Server/.run/ — 커밋하지 않는다.
승인 항목(다음 작업): ⑪(기한 전 관리 작업 건너뛰기, 조건 아래), TSan을 로컬 계측 Core로 재실행(④⑤ 판정, PR 전 필수), ② 잔여 중 transport 조회 lane turn→bind 시점 snapshot. ⑫는 스펙 S2(Errors에서 flow 생성 안 함, sol 반려 0) 확정 뒤 그 스펙대로. request_task 층(public header)은 보류.

## 2차 리뷰 (감독자, 2026-10-15) — a6b373d8d9..a350159aa8
- C-F1 a6b373d8d9 승인: topology가 아는 target은 조회 없이 판정(object_client→NotFound), 모르는 target만 main의 Location Store 판정(같은 의미 복원). 시험: 연결 안 된 Object Client RID send·request NotFound, store 조회 2회.
- C-F2 6d03dd1e00 승인: state_t 소멸자가 남은 chain을 next를 먼저 떼고 해제(item 소멸자가 owner 반납).
- ② 5b1a4ac738 승인 조건부: mesh transport는 bind 시점 불변 index를 atomic load, route channel 이름은 index로 판별 후 lane에서 실제 객체. C-Q1: publish_node_transports는 copy-modify-store라 동시 publisher가 있으면 갱신을 잃는다 — 모든 호출(channel_runtime.cpp:758, channel_runtime_manager.cpp:186,:202)이 같은 lane 위인지 확인 필요.
- ⑫ f4f2255174 승인: flow_context_t::tracing 한 곳(04-flow-correlation §4 :91-94, PR #1821). 시험 2건이 Normal을 설정하도록 바뀜 — 스펙이 propagation을 Normal 이상으로 옮긴 결과라 정당. #1821 머지 뒤에만 이 PR 머지 가능.
- ⑪ 46660e1c94 조건부: 다섯 소유자가 다음 기한의 early bound를 lane에서 게시. 기한을 더 이르게 만드는 모든 쓰기에서 due_now로 되돌려야 하는데 누락 1건 C-F3: public_host_runtime.cpp:3866 — try_emplace(:3842, due_now) 뒤 reply_relocation_ready 송신과 다음 lane turn 사이에 관리 pass가 돌면 next_finalize_at=={}인 attempt를 빼고 bound를 게시(never_due 가능)하고, 이어서 :3866이 next_finalize_at=now+cutover_wait를 써도 bound가 갱신되지 않아 cutover 경고·finalize 재시도가 다른 사건이 bound를 깨울 때까지 미뤄진다. :5322(now로 설정)도 같은 형태(직후 직접 finalize 호출이 있으나 실패 시 재시도가 같은 문제).
- 공유 Core cache 사고: 19:50~20:26 ~/.cache/zlink/core/1.19.0이 TSan Core로 덮임. 그 시간대 다른 job은 Java·.NET뿐(이 cache를 링크하지 않음, 시험 통과). 복구 sha256 일치(agent 보고).

## 3차 (감독자, 2026-10-15) — 375292c208, 546bad8135, merge fe981af3de → PR #1829
- C-F3 375292c208 승인: relocation terminal lane의 모든 deadline·작업 추가 쓰기가 lower_relocation_next_due_on_lane 한 규칙으로 같은 lane turn에서 bound를 낮춘다(lane 단일 writer라 load→store 안전). next_finalize_at writer는 set_relocation_finalize_at_on_lane 하나(:3113, :3561, :3865, :5321). 시험 verify_relocation_finalize_write_lowers_published_bound(대기 없음, 낮추는 줄 제거 시 실패 확인). 다섯 소유자 쓰기 위치 전수표는 m1672c-report §12.1.
- C-Q1 546bad8135 승인: route channel 초기화(channel_runtime_manager.cpp:186,:202)는 lane 밖, bind(:758)는 lane 위 → index 게시를 compare-exchange 재시도로, 변경 람다는 입력을 복사해 재시도에 같은 변경.
- merge fe981af3de 충돌 없음, C++ 변경 없음. unit·contract 155/155, format 통과.
- PR #1829: 0.30.1 태그·스펙 #1821 머지 뒤. 감독자 승인.
- zlink #1829 반려 2건(12:57): (1) 본문에 job queue 재작성 근거·판정 위치·수치 누락 (2) m6a wait_for_activity 단언 삭제로 permit fence 단언이 DATA 도착 전에도 통과(공허) — 감독자가 '더 강한 관찰로 대체'라 승인한 것은 잘못. 수정 지시.
- ab95cc6241 감독자 승인: 두 fence 단언 앞에서 ingress poller가 POLLIN을 보고할 때까지(소비 없이, 관리 deadline으로 깨면 시험 deadline 안에서 반복) 대기 후 단언. DATA 없는 임시 빌드에서 옛 시험 통과·새 시험 실패 확인(agent 보고). PR 본문에 job queue 근거(08 :182-183, 04-application-job-queue :47-57,:165-166,:228-235,:269,:327-328)와 수치 추가.
- zlink 재리뷰 승인 #1829 (13:09).
- 머지 전 확인(zlink 2026-10-15 14:35): #1761 C++(framework-cpp/1761-route-change)이 HELLO 재전송을 tick_liveness 안 submit_pending_hellos_on_lane에 둠. #1829 ⑪은 tick_liveness가 _liveness.due(now) 아니면 즉시 반환 → admission 전 peer는 registry에 없어 due가 안 됨 → HELLO 재전송 멈춤. main merge 때 HELLO 대기 route 등록이 같은 bound를 낮추게(판정 한 곳: liveness due) 맞춘다. framework-cpp/1672-per-message-cost-2도 같은 처리.
- 2026-10-16 main merge 7e253ddfe5(#1851 #1761 C++ 등) 감독자 승인: 충돌은 main 의도 채택(admission 전 application 처리는 #1851의 수신 판정 하나 — 브랜치 쪽 구 not-connected reply 블록 삭제), 브랜치의 message_t reply·apply_route_snapshot(readiness)·permit fence(pump_one :2652) 유지. 의미 충돌 수정: liveness registry set_admission_pending(HELLO 필요 route 있으면 next_due=immediately_due, 아니면 재게시) — admit·route snapshot·HELLO pass에서 refresh_admission_due_on_lane, 판정은 due 한 곳(1→1). 시험: pending HELLO liveness(가상 시각, 수정 전 red) + HELLO·FIFO 100/100, unit·contract 162/162, format.
- zlink 반려(17:34): set_admission_pending(true)가 next_due=immediately_due → ADMIT 대기 중 매 회전 깨어나 HELLO 폭주(바쁜 대기). MeshNode §7.1(03-mesh-node.ko.md:274) '기존 liveness 관리 주기마다'. 감독자 리뷰가 '메시지당 아님'만 보고 응답 없는 상대의 반복 빈도를 확인하지 않음.
- 2026-10-16 f27a338326 감독자 승인(zlink 반려 반영): HELLO 대기 중 다음 기한 = 마지막 HELLO 시각 + probe 주기, 판정 publish_next_due_on_lane 한 곳(immediately_due 삭제). 시험: 주기당 10회전·4주기에서 HELLO 정확히 4회(수정 전 red). unit·contract 162/162. 참고: set_admission_pending의 시작 시각은 clock_t::now()(운영 tick도 steady_clock이라 일치) — registry의 다른 경로처럼 now를 인자로 받는 정리는 후속 후보.
- zlink 재리뷰 승인 f27a338326 (17:58). 머지는 태그·#1821 뒤.

## §4.4 인계 리뷰 (감독자, 2026-10-16) — framework-cpp/1672-per-message-cost-2: e11aa0c13d, 2003ddd563, d9b32811cf (codex sol high, 1.41M 토큰)
- e11aa0c13d 승인: offload_executor에 역할 목록(_roles)과 _role_active. worker는 _roles를 _queue보다 먼저, try_assign_role은 _mutex 아래 `_idle_workers > 대기 역할 수`일 때만(독점 배정), 인계로 worker 생성 없음. 회계: 역할 집으면 _role_active·_active 증가, 인계 성공 시 try_assign_role이 _role_active 감소(그 worker는 application으로), 인계 없이 종료 시 끝에서 감소. drain_application은 `_active == _role_active`까지(역할은 살아 있게), drain_until은 역할까지 0. 역할 실행은 application 예외 경계 밖(terminate 정책 유지), 인계 뒤 application turn은 run_application_turn으로 감쌈.
- 수신 역할 run_until_handoff(mesh_node_host_service.cpp:1777-1835): rotate(대기·drain·수신 회전)가 끝난 뒤에만 인계(§4.4 '이전 자원의 wait·drain과 receive turn이 끝난 뒤'), 인계 성공 시 나머지 owner·completion을 먼저 기존 경로로 넘기고 첫 하나만 실행(결정 §5 엄격 해석). 인계 뒤 옛 holder는 지역(completions·ready)과 읽기 전용 멤버만 사용 — 새 holder의 collected와 공유 없음.
- 2003ddd563 승인: thread_local collector는 rotate 동안만, run_one은 completion_dispatcher_current 설정·예외 격리·release_owner(dispatcher와 같음), 종료 시 collected.publish.
- 결과(보고): request-serial 전환 2 → 0.0038/msg, send I4 0.6286(미달 — 보고서 선택지), unit·contract 157/157, TSan head 전용 0.
- 남은 일: #1829 브랜치(main merge·HELLO 수정 포함) merge 후 재시험 — 단독 측정(p0301) 끝난 뒤.
- 결정: send I4는 선택지 1(serial 목표 달성 채택, send 잔여 0.63 기록) — send에 추가 설계가 필요한지는 단독 측정(#1640/p0301) 뒤 판단. TSan이 잡은 Core poller registry 종료 시 UAF는 base에서도 재현 → 이슈 등록.
- 2026-10-16 #1829 merge 0b7732bbfe 감독자 승인: 텍스트 충돌 0, merge 뒤 #1829 대비 diff = 브랜치 3커밋과 같은 13파일(1120+/322-) 확인. 인계 holder 대기(mesh_node_host_service.cpp:2041)는 -1을 넘기지만 next_activity(관리 결과)로 public_host_runtime.cpp:6549-6556에서 줄임 → HELLO probe 주기 상한 유지. 시험 unit·contract 158/158, HELLO 4주기, m6a, 인계 40회, TSan 4/4. 계측 serial 0.0000, send I4 0.68/0.66/0.75(이전 0.63, 같은 원인 — 선택지 1 유지). 후속: spec 08 :351 C++ 실행 자원 표가 §4.4와 어긋남(스펙 PR에서).
