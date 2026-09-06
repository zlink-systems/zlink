# 공통 규칙 (모든 core-rf job 브리프에 포함)

- 저장소 /home/hep7hep7/project/zlink. 각 job은 자기 detached worktree에서 작업한다: `git worktree add --detach ~/project/zlink-work/<job> main` (이미 있으면 재사용). main에 직접 커밋하지 않는다. 커밋하지 않는다 — 감독관이 리뷰 후 커밋한다. 결과는 worktree의 diff와 보고서로 남긴다.
- 빌드는 worktree의 dev 트리만: `cd <worktree> && JOBS=6 scripts/build-core.sh dev` (RelWithDebInfo, LTO OFF, 테스트 ON). release/LTO 빌드 금지. 읽기·설계·편집·빌드·ctest는 바로 해도 된다(빌드는 JOBS=6).
- 절대 금지: `core/include/**`·`core/src/libzlink.vers` 변경(공개 인터페이스·ABI·export), 공개 계약 테스트(core/tests의 integration/contract/C 테스트) 기대값 변경, 새 옵션·플래그·규칙 추가, 계약 동작(completion·READY/DISCONNECTED·POLLIN/POLLOUT level·WRITABLE wake의 순서와 조건) 변경. 계약을 바꿔야만 가능하면 **거기서 멈추고** 보고서에 D 항목(깨지는 스펙 절, 얻을 이득)으로 적는다.
- 원칙: doc/principal/dev/posddd.ko.md (깊은 모듈, 규칙 수 줄이기, 중복 금지). 설계는 두 가지를 비교하고 고른 이유를 보고서에 적는다. 새 제어점·플래그·상태를 추가하는 방향보다 기존 것을 합치거나 없애는 방향을 우선한다.
- 검증(최소): 변경 파일이 속한 suite와 STREAM 공개 계약 테스트를 `ctest --test-dir core/build-dev -R '<패턴>'`로 5회. pipe·engine·mailbox·mutex를 만졌으면 관련 suite를 TSan 트리(`core/build-tsan`이 있으면 그 구성 재사용, 없으면 `-DENABLE_TSAN=ON` 유사 옵션을 CMakeLists에서 찾아 별도 디렉터리에 구성)로 1회. 전체 ctest는 감독관 게이트 job이 돌리므로 job은 돌리지 않는다.
- **측정 직렬화**: 모든 벤치·valgrind 실행은 `flock /tmp/claude-1000/-home-hep7hep7-project-zlink/a5b31a9a-1a3b-4bcb-a080-53988ed569cb/scratchpad/PERF_LOCK <command>` 로 감싼다(빌드·ctest는 잠금 불필요). 잠금이 몇 분 막혀도 기다린다. 측정 시작 시 load average를 기록한다.
- 성능 확인(STREAM job): worktree의 dev 트리 lib로는 재지 않는다. Release lib가 필요하므로 **worktree에서 `JOBS=6 scripts/build-core.sh release --lib-only`** 로 worktree 안 `core/build`를 만든 뒤, with_stream 러너를 worktree 경로에서 실행한다: `./bindings/c/bench/with_stream/run_benchmarks.sh --stack zlink,asio --size all --ccu 1000 --runs 1 --build-dir <worktree>/bindings/c/build`. 기준값은 doc/plan/core-refactor-stream-perf-0.17.0-plan.ko.md §7.1 Phase 0 행(64 B 268.9 / 1024 B 243.0 / 64 KiB 30.4 kops, asio 322.0 / 316.4 / 39.2). 측정 중에는 다른 빌드를 돌리지 않는다.
- **모든 빌드·테스트·측정 명령은 포그라운드로 실행**한다(백그라운드 + 알림 대기 금지 — 알림을 기다리다 멈춘 job이 있었다). 긴 명령은 timeout을 넉넉히 준다.
- **메모리 규칙(2026-09-07 07:20, WSL 크래시 뒤)**: 이 머신은 11 GB다. dev 빌드는 `JOBS=4`, **동시 빌드 2개까지**(빌드 전 `pgrep -c -f 'ninja'`가 2 이상이면 60 s 대기), valgrind는 빌드와 동시에 돌리지 않는다. 감독관도 apply job을 동시에 2개까지만 빌드 단계에 둔다.
- 상한 1.5 h. 넘으면 증명한 것·남은 후보만 보고하고 멈춘다. 게이트·측정 루프 금지(측정은 before 없이 after 1회, 필요하면 2회).
- 보고서: doc/plan/c016-worklog/core-rf-<id>-summary.md (한국어). 항목: 결과(수치), 변경 파일, 설계 비교와 선택 이유, 실행한 테스트와 남은 실패, 성능 표, 재확인한 스펙 절과 "어느 문장도 다른 동작이 되지 않았다" 확인, 변경 분류 한 줄(A 계약 적응 / B 기존 결함 / C 우회 / D spec gap), 멈춘 지점(있으면). 진행 파일 doc/plan/c016-worklog/progress-<id>.md 를 3분마다 갱신(worktree 밖, 메인 저장소 경로에 직접 씀).
