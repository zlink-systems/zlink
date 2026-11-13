Refs #1639

C++ Framework 수신 경로 1단계(승인된 cpath 설계 §7의 1행): binding의 `received_t`를 record의 유일한 저장소로 쓰고, native RID·원래 reply token으로 응답하며, service 분류를 한 번만 하고, 수신 회전마다 port lease를 한 번만 잡습니다. 코루틴·mailbox claim/settlement·dispatcher·scope·wake 정책은 바꾸지 않았습니다(다음 단계 범위).

## 변경 분류
- B(내부 표현·소유권 전달). public API·wire 형식 변경 없음
- 스펙 근거: `framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md`
  - :185 I2 claim, :186 I3 classify("record마다 header만 해석"), :187 I4 commit("record는 이동하며 복사하지 않는다")
  - 검색어: `rg -n '수신 회전|I3 classify|I4 commit' framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md`, `rg -n -i 'reply token' framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md`(:2054)
- 스펙 수정 없음

## 결정 소유 (수정 전 → 후)
| 결정 | 수정 전 위치 | 수정 후 소유자 |
|---|---|---|
| native parts·routing id·reply token·route generation 수명 | raw route wrapper, mailbox parts/token, dispatch RID 재구성 | record의 binding `received_t` |
| native reply 라우팅과 token | `raw_route_port.cpp` RID 재구성 | binding typed RID/token reply |
| service kind·owner 분류 | `raw_mesh_node_owner.cpp` ingress + `public_host_runtime.cpp` 재판정 | record의 첫 분류 결과 |
| receive port wrapper 수명 | 회전 시작 + record마다 lifecycle mutex·port 조회 | 회전이 보유한 lease |
| socket/poller close gate | port gate | 유지 |

## 개수 (판정 위치·변환 수, 전 → 후)
| 항목 | 전 | 후 |
|---|---:|---:|
| service-kind 판정 위치 | 2 | 1 |
| 일반 send의 native source RID 변환 | 2 | 0 |
| 일반 request/dispatch/reply의 native source RID 변환 | 4 | 0 |
| K건 회전의 receive lease | 1+K | 1 |
| 일반 receive body deep copy | 0 | 0 |
| domain이 미정인 채 생길 수 있는 record 생성 경로(제품 12, 시험 19) | 31 | 0 |
| 새 queue·timer·completion flag·lock | — | 0 |

시험 helper 11곳과 parts를 채우는 시험 레코드 14곳은 product factory `service_mailbox_record_t::from_parts`로 모았습니다(조건부로 parts를 채우는 3곳과 parts 없는 application 레코드 19곳은 그대로). 시험 전용 product API는 추가하지 않았습니다.

## 검증
- pre-pr(zlink 머신, 1119bde723 + main 486caf71a8, checkout Core·binding 1.23.0): exit 0. Release 빌드 585/585, ctest unit·contract 301/301, `format.sh --check cpp` 통과. 로그 D:/t/jobs/fx/cx/j1639p1-build7/
- SAMPLES·CROSS_LANGUAGE ON configure·전체 빌드 649/649 통과
- `test_cpp_framework_receive_record` 통과(ctest #243): 한 회전 3 request에서 native body copy 0, body materialization 0, service classification 3, port lease 1, record별 pointer·typed RID·generation·원래 token 일치, reply completion 확인
- 이 PR 커밋 45ea3c721e의 tree는 위 검증 커밋 1119bde723과 같습니다(WIP 커밋 10개를 하나로 정리)
- kairos 서버의 pre-pr는 Core 1.24.0 gate 측정 중이라 돌리지 않았습니다. 위 결과는 PM 머신 실행입니다
- 음성 대조(`ZLINK_RECEIVE_RECORD_NEGATIVE=copy|clone|classification|lease|token|drop|malformed`): PM 머신 build7 바이너리에서 7종 모두 대응하는 정상 단언이 실패함을 확인(baseline 통과). 출력 D:/t/jobs/fx/cx/j1639p1-build7/negative/ (baseline.log, copy·clone·classification·lease·token·drop·malformed.log), 실행 스크립트 D:/t/jobs/fx/cx/j1639p1-build7/neg.sh. 리뷰 수정 push 뒤 새 바이너리로 다시 돌려 갱신합니다
- 성능(셀 측정): gate → xbp → 24칸 한 세션 뒤 순서로 kairos 서버에서 측정해 별도로 올립니다

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01D9jfewPT9LTVRsk1VFWTdu
