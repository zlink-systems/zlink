Closes #63

## 무엇을 바꿨나

Core C API의 **part 단위 공개 API를 전면 제거**하고 **whole-message(배열+count) send/recv로 통일**했다. 한 record를 여러 호출로 만들던 표면이 만들던 미완성-record 상태와 "한 record 첫 part~FINAL 같은 thread" 계약·`BUSY`·부분 재시도 규칙이 함께 사라진다.

- **Core**
  - 신설: `zlink_send`·`zlink_send_rid`·`zlink_request`·`zlink_reply`·`zlink_publish`·`zlink_recv`·`zlink_router_recv`·`zlink_subscribe`·`zlink_xpub_recv` (모두 caller-제공 `zlink_msg_t[]` + count).
  - 제거: `zlink_*_part` 8개 + `zlink_xpub_recv_part` + 공개 `zlink_part_flag_t`/`ZLINK_PART_MORE`/`FINAL`.
  - 유지: `zlink_stream_recv_packet`, `zlink_multipart_close`. STREAM RAW 수신=`zlink_recv`(1 part), STREAM 송신=count 1.
  - 내부 rename: `recv_router_message_direct`→`recv_router_record`, `recv_dealer_message_direct`→`recv_dealer_record`.
  - core/tests 547 call site 이관, 불가능해진 시나리오 테스트 17개 제거.
- **바인딩 7종**(c/cpp/dotnet/go/java/node/python·rust): 내부 send/recv를 whole-message 1회 호출로 전환. **공개 시그니처 불변**. C perf/bench/samples/tests도 이관.
- **문서**: Core 스펙(ko/en)·guide·바인딩 spec/guide 전량 whole-message로 갱신(작성원칙 준수, part 참조 0).

계약 결정: D63-1..D63-8 (`doc/plan/issue-63-worklog/`). 설계·조사 근거: `doc/draft/core-whole-message-recv-api.ko.md` §7.

## 검증

- **Core ctest**: `ctest --test-dir core/build-dev -E hotpath_gate` → **214/214 통과, 0 실패**. (`hotpath_gate`는 dev(LTO OFF) 빌드의 Ir 아티팩트로 제외 — release-gate 전용.)
- **공개 표면 계약**: `check_public_surface.py` → **PASS** (functions=99, exports 일치, 제거 식별자 부재, ko/en C블록 동일).
- **바인딩 계약테스트**(병합 브랜치, `ZLINK_CORE_SOURCE=local` core/build-dev): **cpp·rust·java·node·dotnet·go 6종 PASS**. python은 각 마이그레이션 job에서 통과(d081603bbd, 235 tests)했으나, 이 worktree의 로컬 빌드 venv가 `setuptools`/Core payload 동기화를 갖추지 못해(build-wsl python 경로 미실행 — env, 코드 아님) 최종 sweep에서 C-extension 재빌드는 생략. 각 바인딩은 마이그레이션 job에서도 이미 계약테스트를 통과함(cpp 19/19, node 152/152, dotnet 238/238, java all, go/rust PASS).
- **문서 검사**: `check_doc_links.py`(core-doc)·`check_prose_neutrality.py`·`check_doc_tabs.py core` PASS.

## perf (multi routed, before=Core 0.17.4 baseline → after=이 브랜치 whole-message)

바인딩 throughput이 4개 측정 언어 모두 대폭 향상(소·중 사이즈 SENDSEND에서 최대). 비대상 회귀 없음.

| 바인딩 | 개선 범위(Δ throughput) |
|---|---|
| cpp | +8 ~ +187% |
| java | +16 ~ +240% (1셀 −8% 노이즈) |
| dotnet | +27 ~ +187% |
| node | +18 ~ +284% (보수적) |

메시지당 native 경계·part-flag·part 루프 제거 효과. 상세·주의(baseline 0.17.4 confound 등)는 `doc/plan/issue-63-worklog/perf-results.ko.md`.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01Df7vbMq5yraG6siDn8PAUH
