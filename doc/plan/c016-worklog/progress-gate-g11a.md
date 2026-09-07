# gate g11a 진행

- 2026-09-07 시작. `core-rf-GATE.prompt`, `_common-rules.md`, `doc/AGENTS.md`, G-11a 요약 §8을 확인했다.
- main은 `a2b087ff0c`이며 `origin/main`과 동일하다. `git pull --rebase -q`는 다른 작업의 `doc/plan` 진행 파일 변경 때문에 Git이 거절했으나, 원격 선행 커밋은 없다.
- G-11a worktree(`08da256f1e`)의 두 파일 patch를 추출해 main에 `git apply --3way`로 clean 적용했다. 충돌 없음, 공개 헤더/ABI diff 없음.
- 다음: 빌드 대기 규칙 확인 뒤 dev build와 지정 ctest 패턴을 실행한다.
- 개발 빌드(`JOBS=4`)는 성공했고 전체 ctest(`-j2 --output-on-failure`)를 실행 중이다. 다른 job의 build/ctest와 겹치는 동안 측정은 시작하지 않는다.
- 전체 ctest 완료: `hotpath_gate` 1건은 dev 트리에서 예상되는 실패이며, 나머지 207건은 통과했다. 지정 61개 suite의 5회 반복은 1/5 통과, 2/5 실행 중이다.
- 지정 suite 반복은 4/5까지 통과했고 5/5를 실행 중이다. `test_stream_socket_recv_multiclient_ready_regression`도 각 완료 반복에서 통과했다.
- 지정 suite 61/61 ×5가 모두 통과했다. lost-wake 6개 ×10은 `test_wake_invariants` 반복을 포함해 실행 중이며 현재까지 실패 없다.
- lost-wake 6개는 모두 10회 통과했고 close/release는 50회 통과했다. ABI/mirror도 통과. hotpath 5셀은 4 PASS, `dealer_router_reqrep_inproc`은 reference 대비 0.9482로 FAIL(개선 폭 +5% 초과)이다. release/gate build 뒤 성능 셀을 측정 중이다.
- 완료. 결과는 `gate-g11a-summary.md`에 기록했다. 포팅 patch는 main working tree에 유지한다. hotpath 1셀은 개선 폭 정책으로 FAIL이고, with_stream 기본 release download 404는 `ZLINK_CORE_SOURCE=local`로 재측정했다.
