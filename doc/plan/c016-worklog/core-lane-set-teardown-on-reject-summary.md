# Core count-2 재시도 lane 결합 진단

**미완료 / 승인 보류.** `test_reject_retry_tcp`는 서로 다른 connect intent의 새
Completion lane끼리도 충돌한다. 이번 gdb 표본에서는 기존 두 lane set의 session pipe가
모두 종료되고 engine이 없는 상태를 확인한 뒤에도 실패했다. 따라서 “이전 attempt의
Completion 잔류”만을 원인으로 하는 수정으로 이 테스트의 성공을 보장할 수 없다.
관찰한 순서는 종료 callback → 새 READY이며, 모든 종료 뒤 새 TCP 연결을 시작하는
장벽을 적용한 실험은 아니다. 독립 intent의 충돌을 장벽만으로 배제할 수 없다는 판단은
코드와 계약에 따른 추론이다. 요청한 lane-set teardown 구현은 추가하지 않았다.
아래 계약 판정이 남는다.

작업 기준은 `/home/hep7/project/zlink-core-b`의 기존 D-094 작업본이다. 기존 runtime·test
변경 8개 파일을 그대로 보존했다. branch 전환, commit, spec·bindings·framework 수정은
없다. main 변경은 이 보고서이며, 빌드는 worktree b의 `core/build-dev`만 사용했다.

## 원인과 공개 API 재현

`core/tests/integration/test_router_same_socket_reconnect_policy.cpp:356-370`은
`127.0.0.1`과 `localhost`로 만든 두 connect intent를 유지한 채 server의 기존 RID를
`zlink_disconnect_rid()`로 종료한다. 두 intent 모두 재시도를 수행한다.
`socket_base_endpoint.cpp:480-510`은 다른 endpoint 문자열마다 별도 pair ID와
`transport_pair_state_t`를 만든다.

`core/src/runtime/sockets/common/socket_base_api.cpp:84-96`은 binder에서 RID별
미완성 pair 하나만 조회한다. 같은 RID로 도착한 connection은 어느 connector intent에
속하는지 구분하지 않고 그 pair를 재사용한다. `:338-342`는 같은 lane의 두 번째 pipe를
거부하고 `:447`에서 READY protocol error를 게시한다. 이 duplicate 검사는 ZMP §4.1과
일치한다. 서로 다른 set을 같은 pair로 결합하는 선행 단계가 문제다.

### gdb 증거

`/tmp/zlink-core-laneset-gdb-closed.log`는 reset, session pipe 종료, engine READY,
socket attach와 protocol error에 breakpoint를 걸어 실행한 원본이다.
`/tmp/zlink-core-laneset.gdb`에 재현 명령이 있다. 로그의 session `gen=2`는 reset이 이미
증가시킨 local generation이며, 종료된 old pipe는 앞선 RESET 줄의 pipe 주소와 대응한다.

| 항목 | `localhost:7123` intent | `127.0.0.1:7123` intent |
|---|---|---|
| Connector pair ID | `5061934128356295738` | `3921268964842168273` |
| Old Application session pipe 종료 | 로그 :86, engine NULL | 로그 :107, engine NULL |
| Old Completion session pipe 종료 | 로그 :84, engine NULL | 로그 :114, engine NULL |
| 새 Completion local generation | 2 | 2 |
| 새 Completion TCP source port | 39604 (:120-125) | 39620 (:138-142) |
| Binder가 부여한 pair ID | `8139533077041933910` | `8139533077041933910` |

Binder의 이전 accepted Application·Completion session도 로그 :68/:73과 :96/:109에서
종료됐다. 이후 :118부터 새 READY가 기록되고 :147에서 duplicate Completion 오류가
발생한다. 이전 Completion을 재사용한 것이 아니라 **새 generation에 속한 서로 다른
intent의 Completion 둘을 같은 accepted pair로 결합한 표본**이다.

별도 표본 `/tmp/zlink-core-laneset-gdb.log`에서는 connector pair
`16005846700768983150/2`의 Completion(source port 21984)과
`16666081849907604183/2`의 Application(source port 22000)을 binder pair
`15301247059579718717/1`로 결합한 뒤, 후자의 Completion(source port 22004)을
duplicate로 거부했다. 이는 교차 결합이 Application/Completion 조합도 만들 수 있다는
직접 증거다. 기존 `/tmp/zlink-core-d094-gdb-pair.log`의 오류 지점만으로는 이 intent
귀속까지 구분할 수 없었다.

## 종료 소유자와 대안

현재 `session_base.cpp:788-811`은 자기 pipe 종료를 요청하고 자기 connector를 다시
시작한다. `options.hpp:67-79`의 `begin_reset()`은 같은 intent의 generation 증가를
공유하지만 두 engine의 종료 완료를 기다리는 장벽은 아니다. Sibling pipe 종료는
`socket_base_api.cpp:1815-1842,1930-1937`의 최종 pipe 해제 경로에 있다.
이 구조의 §4.1 준수 문제는 남아 있으며 이번 조사로 해결됐다고 판정하지 않는다.

Binder의 논리 종료는 `socket_base_api.cpp:1733-1741`에서 해당 accepted RID association을
해제한다. 그러나 이 해제는 서로 다른 새 intent가 같은 RID로 도착하는 경우를 구분하지
못한다. 종료된 incomplete pair의 queued sibling admission도 별도 검증이 필요하다.

| 대안 | 판단 |
|---|---|
| 한 intent의 두 lane 종료 완료 후 같은 다음 generation으로 재개 | §4.1의 기존 요구. 현재 결함 조사·수정 대상이지만 위 동시 intent 표본을 배제하지 못한다. 이번에는 구현하지 않았다. |
| 서로 다른 동시 count-2 intent를 구분하는 결합 계약 확정 | 재현된 교차 결합을 직접 다룬다. Wire property 또는 동시 intent 허용·admission 규칙의 판정이 필요하므로 감독에게 이관한다. |

재시도 지연·budget 증가, 중복 lane 허용, assertion 완화와 endpoint 특례는 적용하지
않았다. D-094의 REJECT/HANDOVER 두 정책과 D-092의 application recv 없는 종료 관찰은
기존 작업본 그대로다.

- **소유 계층:** Core connector session/pair lifecycle와 binder accepted lane 결합. Framework 소유 동작이 아니다.
- **Spec 조항:** ZMP `protocol/01-zmp.ko.md` §4.1(:187-202, :249-254), 요약 :506-521; socket `README.ko.md` §4(:159-169), D-092·D-094. Local generation은 wire property가 아니며 두 lane의 READY Routing-Id는 같다.
- **교차언어 대조:** 재현은 공개 C API 테스트이며 모든 binding이 같은 Core 구현을 사용한다. Framework runtime은 변경하지 않았다. 언어별 독립 runtime 실행·검증은 수행하지 않았다.
- **변경 분류:** 적용한 runtime 수정 없음. 기존 lane-set 종료 문제는 **B 기존 결함** 조사 대상. 동시 same-RID count-2 set의 결합·허용 계약은 **D spec gap 후보**, 감독 판정 필요.
- **수정 전/후 규칙 수:** 이번 작업 **변화 없음**. 기존 D-094 admission의 3→2 변경을 보존했으며 새로운 예외·상태·타이머를 추가하지 않았다.

## 검증

아래 결과는 추가 runtime 수정이 없는 기존 D-094 작업본에 대한 검증이다.

| 검증 | 결과 | 로그 (`/tmp/` 아래) |
|---|---|---|
| `nice -n 10 env JOBS=4 scripts/build-core.sh dev` | PASS | `zlink-core-laneset-build.log` |
| `ZLINK_TEST_CASE=test_reject_retry_tcp`, `--repeat until-fail:10` | 첫 실행 FAIL, :79 protocol event 단언 | `zlink-core-laneset-before.log` |
| `test_router_same_socket_reconnect_policy` 전체, `--repeat until-fail:10` | 첫 실행 6 case 중 TCP retry만 FAIL, 5 PASS; 반복 중단 | `zlink-core-laneset-repeat.log` |
| 요청한 표적 8개 | 8/8 PASS, 45.48초 | `zlink-core-laneset-targeted.log` |
| 전체 ctest 1회, hotpath 제외 | 176/176 PASS, 232.19초 | `zlink-core-laneset-full.log` |
| `git diff --check`, 기존 diff 보존 검사 | PASS; 입력 diff와 바이트 단위 일치 | — |

전체 ctest에서는 해당 회귀도 통과했다. 앞선 반복 gate 실패는 간헐 실패로 남으며,
이 한 번의 전체 통과를 수정 완료로 판정하지 않는다.

표적 8개는 `test_router_reject_duplicate`,
`test_router_reject_disconnected_without_app_recv`, `test_socket_disconnect_boundary`,
`test_router_reciprocal_handover_lanes`, `test_transport_matrix`,
`test_monitor_connection_identity`, `test_multi_socket_contract_regressions`,
`test_inproc_pending_connect_rejected_at_attach`다. Hotpath와 binding/Framework gate는
이번 요청 범위에서 제외했다.

## 산출물

- 보고서: main의 `doc/plan/c016-worklog/core-lane-set-teardown-on-reject-summary.md`.
- 통합 패치: `/home/hep7/project/zlink-core-b/core-d094-plus-laneset.patch`.
  **기존 D-094 diff만 포함하며 lane-set 수정은 포함하지 않는다.**
- 패치 SHA-256: `c24a9bb017f5d81b794f3a70377339f8f0847802dc5ae0274d868a57a4d24373`.
  기존 untracked 산출물을 일회성 `core.excludesFile`로 제외하고 `git add -N .` 뒤
  `git diff HEAD`를 저장했다. `git apply --reverse --check`를 통과했다.
- 기존 diff 보존 기준: `/tmp/zlink-core-laneset-input.patch`.
- 문서의 원칙 준수·코드 부합 독립 리뷰를 완료하고, 관찰과 추론의 구분 및 spec 표현
  관련 지적을 반영했다.

## BLOCKERS

1. **Same-RID 동시 count-2 set의 결합 계약 판정.** 위 gdb 표본은 old lane 종료 후에도
   서로 다른 intent의 새 Completion이 충돌함을 보여준다. 현재 spec은 두 lane의
   RID·socket type·count 일치를 요구하지만, 이 동시 intent 시퀀스의 처리 규칙은
   감독의 추가 판정이 필요하다.
   Wire 식별자 추가나 새 admission·직렬화 정책을 이번 B 수정으로 임의 도입하지 않았다.
2. **요청한 lane-set teardown 구현 미완료.** Session의 재개 전에 두 lane의 종료 완료를
   보장하는 소유자 수정과 binder의 종료된 incomplete pair admission 검증이 남는다.
   기존 분석 방향만으로 TCP retry gate를 green으로 만들 수 있다고 판정하지 않는다.
3. **TCP retry 회귀 실패 유지.** 전체 반복 gate가 red이므로 본 산출물은 commit 승인이나
   수정 완료의 근거가 아니다. 테스트 조건과 실패 단언은 변경하지 않았다.
