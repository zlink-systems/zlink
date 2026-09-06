# R3 적용 job 요약 — pipe/session_base 소규모 정리

worktree `~/project/zlink-work/r3` (detached `3d84da1fd1`). 커밋하지 않음. 변경 15파일 +158/−178.

## 1. 결과

| 묶음 | 항목 | 결과 |
|---|---|---|
| A | `session_base_t::reset()` | 제거(선언 1 + 정의 3행 + 호출 2곳). 본체가 빈 no-op이고, 리포 전체에서 `session_base_t` 상속 클래스는 테스트 픽스처 `contract_zmp_session_t` 하나뿐이며 **오버라이드 0건**, 엔진·디코더 인터페이스에서 `reset()`을 요구하는 곳 0건. 호출 2곳(`session_base.cpp` reconnect / start_transport_pair_reconnect)은 no-op 호출이므로 삭제해도 동작 동일. |
| A | `ZLINK_DEBUG_ROUTER_ROUTE` | 제거(익명 네임스페이스 트레이스 블록 63행 + hot path 호출 4곳 + `debug_log.hpp` include). `doc/` 전수 grep 결과 **지원 knob으로 문서화된 곳 없음** — 나오는 곳은 전부 c016 워크로그의 과거 진단 명령줄과 rf3 요약의 "보류" 기록뿐. `push_msg_internal` 메시지당 분기 1개 제거. |
| B | `pipepair()` 파라미터 10 → 5 | `pipepair_options_t`(NSDMI 애그리게이트, 6필드) 도입. 배열 인자 4개(parents/pipes/hwms/conflate)는 유지, 나머지 6개(불리언 3 + enum 2 + int 1)를 구조체로. 호출처 전수 갱신: src 4곳, 테스트 27곳. 동작 변경 없음(각 필드 기본값이 기존 기본 인자와 동일). |
| 1 (분할) | pipe.cpp 4112행 분할 | **착수하지 않음**. 사유: (a) WSL 크래시·리부트와 G-0 게이트 대기로 1.5 h 캡 내 검증 시간이 남지 않음, (b) 더 중요한 설계 이유 — `pipe.cpp` 상단 익명 네임스페이스의 `pipe_debug_log`, `consume_if_delimiter`, `probe_normalized_head`, `head_reclassify_{idle,armed,queued}`, `pending_peer_weight_unset`, `g_stream_packet_allocation_failpoint`와 1105행의 두 번째 익명 네임스페이스가 분할 대상 함수들에 걸쳐 쓰인다. 파일을 나누면 이들을 공유 내부 헤더로 빼면서 **internal linkage → external/inline**으로 바뀌므로 "verbatim 코드 이동"이 성립하지 않고, 브리프가 우려한 인라이닝 변화가 실제로 발생할 수 있다. 순수 이동으로 만들려면 먼저 이 헬퍼들의 소유 위치를 정하는 별도 결정이 필요하다 → 후속 job 권고. |

## 2. 설계 비교와 선택 이유 (묶음 B)

- 후보 1(선택): **NSDMI 애그리게이트 + `const &` 기본 인자**. `pipepair_options_t`에 멤버 함수·생성자를 두지 않았다. 4-인자 호출처(테스트 다수)는 한 글자도 바뀌지 않고, 비기본값 호출처만 이름 붙은 지역 변수로 바뀐다. 새 제어점·규칙 0개(필드 6개는 기존 파라미터 6개와 1:1).
- 후보 2(기각): 체이닝 세터(`pipepair_options_t ().set_lane (...)`). 호출처가 한 표현식으로 남아 짧지만 **필드마다 세터 이름 하나씩 총 6개의 새 이름**이 늘고, 값 하나만 다른 곳에서도 API 표면이 커진다. POSDDD의 "규칙 수 줄이기"에 어긋나 기각.
- 후보 3(기각): `bool`에서 암시적 변환되는 생성자. 호출처 churn은 0이지만 인벤토리가 지적한 위치 기반 불리언 정보 누출이 그대로 남는다.
- `hwms_`/`conflate_`를 구조체에 넣지 않은 이유: 둘은 방향별 배열([0]/[1] 의미가 함수 내부에서 뒤바뀌어 쓰인다)이라 구조체로 옮기면 오히려 의미가 흐려진다. 인벤토리 제안도 동일.

## 3. 실행한 테스트

| 항목 | 결과 |
|---|---|
| dev 빌드 (JOBS=4, RelWithDebInfo/LTO OFF) | 성공, 경고 없음 |
| `ctest -R 'pipe|wake|poll|stream|hwm|flow|credit|router|dealer|pair|session'` 5회 (72 테스트) | 5회 중 4회 72/72 PASS. 3회차에 `test_single_lane_flow_snapshot_accounting` 1건 실패(`test_dealer_router_single_lane_contract.cpp:2842`, 5 s accounting wait 만료) — **기지 간헐**(`decisions.ko.md:139`, `gate-s12-summary.md:16`, `core-rr-reqrep-64k-summary.md:46`에 동일 증상 기록). 단독 `--repeat until-fail:5` 5/5 PASS(0.07 s). |
| lost-wake set `ctest -R wake --repeat until-fail:5` | 5 테스트 × 5회 = 전부 PASS (156 s) |
| TSan | 미실행(파이프 동작 로직 변경 없음 — 삭제·시그니처 변경만) |

## 4. 성능 (축소 callgrind STREAM 셀, S-A 방식, CCU 20 / 1024 B / 15 s)

release(LTO ON) lib를 worktree에서 빌드한 뒤 `flock PERF_LOCK`으로 직렬화. 측정 시작 load average 0.36 / 0.58 / 0.81, 빌드 동시 실행 없음(`pgrep -x ninja` = 0).

| 런 | recv_msgs | Ir 총합 | idle 차감 후 Ir/msg |
|---|---|---|---|
| full #1 | 94,261 | 900,057,928 | **9,509** |
| full #2 | 82,753 | 799,456,670 | **9,616** |
| idle(서버 단독) | — | 3,779,104 | (차감분) |

- 기준 9,474 대비 +0.4 % / +1.5 %. 두 런의 차이가 곧 이 셀의 런간 편차다: 두 점을 직선으로 맞추면 **고정비 ≈ 76 M Ir, 한계비용 ≈ 8,742 Ir/msg** 로, Ir/msg 총계는 valgrind 직렬화로 흔들리는 처리량(메시지 수)에 강하게 좌우된다. 메시지가 적게 잡힌 런일수록 고정비 분모가 작아져 Ir/msg가 커진다.
- **파일 분할을 하지 않았으므로 LTO 인라이닝이 달라질 여지 자체가 없다.** 변경분 중 메시지당 경로에 닿는 것은 `push_msg_internal`에서 트레이스 분기 4개가 사라진 것뿐이고 이는 감소 방향이다. `pipepair_options_t`는 파이프 생성 시점(콜드 경로)에만 쓰인다. 따라서 위 편차는 측정 잡음으로 판단한다.

## 5. 스펙 재확인

- `session_base_t::reset()`은 빈 함수였고 오버라이드가 없다 → 호출 삭제로 실행되는 명령이 하나도 달라지지 않는다.
- `ZLINK_DEBUG_ROUTER_ROUTE` 트레이스는 stderr 출력만 하고 `msg_`/`_pipe`/`_socket` 어느 상태도 건드리지 않는다(읽기만) → 삭제로 completion·READY/DISCONNECTED·POLLIN/POLLOUT level·WRITABLE wake 순서와 조건 중 **어느 문장도 다른 동작이 되지 않았다**.
- `pipepair_options_t`의 6개 필드 기본값은 삭제한 기본 인자와 값이 같고, 함수 본문은 파라미터 이름만 `options_.<필드>`로 치환했다(로직·순서 변경 0). 비기본값을 넘기던 호출처 31곳은 넘기던 값을 그대로 필드에 대입한다.
- `core/include/**`, `core/src/libzlink.vers`, 공개 계약 테스트 기대값: 변경 없음.

## 6. 변경 분류

**B(기존 결함/잔재 정리)** — 죽은 no-op 가상 함수와 문서화되지 않은 디버그 계측 제거, 그리고 내부 API의 파라미터 과다 정리. 계약 적응(A)·우회(C)·spec gap(D) 아님.

## 7. 멈춘 지점 / 인계

- 인벤토리 항목 1·10(pipe.cpp 4112행 분할)은 위 2번 사유로 미착수. 선행 조건: 익명 네임스페이스 헬퍼 6종의 소유 위치 결정(내부 헤더로 뺄지, 각 개념 파일로 나눠 붙일지). 그 결정 없이는 "순수 코드 이동"이 아니다.
- 인벤토리 항목 5(묶음 C, route-binding 캐시)는 브리프대로 제외.
- **파일 겹침 주의**: 묶음 B의 호출처 갱신이 `core/src/runtime/sockets/common/socket_base_endpoint.cpp`의 pipepair 호출 3곳을 건드린다(R4 담당 디렉터리). 해당 파일에서 바꾼 것은 그 3개 호출의 인자 목록뿐이므로 병합 충돌 면적은 최소지만, 감독관 병합 시 R4와 함께 확인이 필요하다.
