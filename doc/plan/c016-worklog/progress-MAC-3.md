# MAC-3 진행

- 08:30 시작. MAC-2 보고서·run 34202650122 로그 확보.
- 실패 상세(34202650122): `saturated=1 accepted_max=26 waiters_blocked=0 max_wait_ms=44 drain_elapsed_ms=2023`.
  → MAC-2가 추정한 "프로브 기아"가 아니라 **waiter가 44 ms에 실제로 깨어남**. 즉 조기 크레딧이 다시 발생.
- 원인 가설: settle 단계가 `zlink_completion_recv(DONTWAIT)` **1회 샘플**로 "recredited==0"을 판정 → io 스레드가 아직 하류로 바이트를 밀고 있는 중이라 몇 ms 뒤 크레딧이 더 온다.
- 방향: settle을 **이벤트 관측**으로 교체 — 전원이 backpressure된 뒤 `zlink_poll(ZLINK_POLLOUT, quiet_ms)`로 정지구간을 기다려 timeout(=아무도 writable이 아님)일 때만 saturated 판정.
- 다음: 550f0e3f6e cherry-pick, 테스트 수정, Linux 검증, macOS 진단 워크플로 3회 반복.
- 08:20 `550f0e3f6e` cherry-pick(`507f5f8d42`) — parallel 그룹 `unittest_flow_state_monitor` 타임아웃 대응.
- 08:25 settle 단계를 이벤트 관측으로 교체(`3178bfde63`): 전원 backpressure 시 `zlink_poll(POLLOUT, 1000 ms)` — 0 반환(=1 s 동안 아무도 크레딧 회복 안 함)일 때만 saturated. 크레딧 회복한 클라이언트만 WRITABLE 소비 후 계속 채움. 기대값·토큰 규칙 불변.
- 08:28 Linux `ctest -R wake --repeat until-fail:5` → 5/5 통과(테스트당 32.9 s, 이전 32.1 s = quiet window 1 s 추가분).
- 08:29 macOS run **34204737552** 시작(scope `^test_wake_invariants$`, `until-fail:3`).
- 제외 정규식은 쓰지 않는다(감독관 결정). 실패 시 Core 경로 조사로 전환.
- 08:37 run **34204737552** 결과: 1회차 PASS(4.19 s), 2회차 FAIL — **증상이 바뀜**. `waiters_blocked=1`(전원 블로킹 확인, 원래 문제 해소), `accepted_min=30 max=39`(깊은 포화), `drain_elapsed_ms=5`, `max_wait_ms=5978`, `recovery_after_drain_ms=5973` → 마지막 단언 `recovery_after_drain_ms < wake_timeout_ms(2000)` 위반.
- 분석: Linux 로컬 계측(DIAG-MAC3) — `accepted_min=max=16`, `drain_elapsed_ms=15910`, `recovery_after_drain_ms=0`. Linux는 drain 자체가 네트워크 바운드라 크레딧이 drain 중에 도착한다. macOS는 서버 인바운드 큐(RCVHWM 1 MiB×100 = 최대 100 MB)가 미리 차 있어 drain이 **메모리에서 5 ms에 끝나고**, 그 뒤 51 MB가 전송로를 건너야 크레딧이 돌아온다 → 6 s.
- 결론: 저수지(서버 수신 큐)가 drain과 그 drain이 유발할 크레딧을 분리시킨다. 서버 RCVHWM을 2레코드(128 KiB)로 낮춰 drain이 실제로 wire에서 끌어오게 함(`c98290dd19`). Linux 재확인: 16/16, drain 14.8 s, recovery 0 — 변화 없음.
- 08:45 macOS run 대기 중.
- 08:47 run **34205855167**: `test_wake_invariants` **3/3 통과**(3.47/3.71/3.65 s). (run 전체는 parallel 그룹에 매칭 테스트 0개라 exit 1 — 스코프 탓)
- 08:53 run **34206451147** full: parallel **57/57 통과**(cherry-pick 효과), serial **148/149** — `test_wake_invariants` **통과**, 대신 신규 플레이크 `test_stream_packet_progress:84 test_shutdown_during_drain: transport did not queue all fragments` 1건. 이전 3개 run에서는 통과하던 테스트.
- Linux `-R wake --repeat until-fail:5` 2회(총 10반복) 전부 통과. MAC-3.patch 작성 완료.
- 다음: test_stream_packet_progress 조사.
- 09:05 `test_stream_packet_progress` 원인: `await_input()`이 `zlink_monitor_status`를 `yield()` 스핀으로 폴링 — 매 폴이 모니터 상태 락을 잡아 262k개 1바이트 WS 프래그먼트를 흡수해야 하는 io 스레드와 경합. 3코어 러너에서 3 s 마감 초과. `msleep(1)` 폴링으로 교체(`2966e2e2dd`). Linux `until-fail:5` 통과.
- 09:10 full macOS run 재실행 대기.
- 09:14 run **34207768327** full: serial 148/149 — `test_stream_packet_progress` 재실패(msleep 수정으로 해결 안 됨), parallel 56/57 — `unittest_asio_transport_writev_lifetime:165 Expected 0 Was 1` 신규 1건.
- 09:21 run **34209046724**(두 테스트만, until-fail:3): **둘 다 3/3 통과**. 단독에서는 재현 안 됨 → full run 부하/순서 의존.
- 09:24 `await_input` 실패 메시지에 `queued/expected/last_progress_ms` 추가, 워크플로에서 TCP 샘플러(백그라운드 netstat 루프) 제거 후 full run 재실행.
- 09:35 run **34209700208** full: **serial 149/149, parallel 57/57 — 전부 통과**(388.5 s / 21.5 s). run 자체는 Summary 스텝의 `[ -f ] &&` 마지막 종료코드 때문에 red.
- 09:40 `unittest_asio_transport_writev_lifetime` 경합(EAGAIN 직후에도 커널이 공간을 되돌려 2바이트 write가 인라인 완료) 을 `fill_send_buffer`에서 POLLOUT 정지구간 대기로 제거 + Summary 스텝 exit 0. 확인 run 대기.
- 09:51 run **34210988899** full: **serial 149/149, parallel 57/57, run conclusion success**. macOS ARM64 전부 통과.
- 09:55 Linux 최종 확인 8개 테스트 `until-fail:3` 통과. MAC-3.patch 갱신, 보고서 작성 완료. 종료.
