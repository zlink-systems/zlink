# gate-mp 진행

- 2026-09-08 03:09 KST: 재실행 지시를 확인했다. MP patch는 이미 main index에 staged되어 있어 적용 단계는 생략했고, `core/doc/spec/**` 수정은 제외했다. staged Core 목록 30개(신규 `test_writable_resubmit_from_other_thread_while_sequence_open.cpp` 포함)를 보존해 보고서에 기록할 예정이다.
- 다음: dev build, 전체 ctest 1회, 변경 suite 3회, ABI mirror, release/hotpath, with_stream을 순서대로 실행한다.
- 2026-09-08 03:12 KST: `JOBS=4 scripts/build-core.sh dev`가 포그라운드에서 96%까지 진행 중이다. 이번 staged 소스와 변경 테스트 대상은 모두 컴파일·링크 단계에 들어갔고, 빌드 오류는 아직 없다.
- 2026-09-08 03:15 KST: dev build는 성공했다. 전체 `ctest --test-dir core/build-dev -j2 --output-on-failure` 1회가 진행 중이며, 현재 완료된 초기 5개는 모두 PASS다.
- 2026-09-08 03:19 KST: 전체 CTest는 210개 중 기능 209개 PASS, `hotpath_gate` 1개 FAIL(개선 폭으로 reference 범위 밖; 지시상 hotpath 제외 정상)로 끝났다. 신규 writable-resubmit 두 case도 PASS했다. 변경 정규식 suite 3회로 전환한다.
- 2026-09-08 03:22 KST: 변경 정규식 suite는 회차당 97개다. 1회차가 진행 중이며, 완료된 wake/request/router/reply/helper 대상은 모두 PASS다.
- 2026-09-08 03:25 KST: 변경 suite 1회차는 97/97 PASS(140.63초)로 완료했다. 2회차 진행 중이며 첫 wake-invariant도 PASS다.
- 2026-09-08 03:28 KST: 변경 suite 2회차도 97/97 PASS(143.79초)로 완료했다. 3회차가 진행 중이며 첫 세 장기 대상은 PASS다.
- 2026-09-08 03:31 KST: 변경 suite 3회차도 97/97 PASS(141.08초)로 완료했다. 기능 검증 3회 반복은 291/291 PASS다. ABI diff와 12개 mirror 확인 후 release 단계로 간다.
- 2026-09-08 03:34 KST: staged public diff는 비어 있고 raw `zlink.h`·`zlink_enum.h`·`zlink_errno.h`의 C/C++/Go/Rust mirror cmp 12/12가 일치했다. `JOBS=4` release lib-only 빌드도 성공했다. PERF_LOCK hotpath 단계로 진행한다.
- 2026-09-08 03:37 KST: hotpath 5셀은 완료했다(load `3.72/1.68/1.00`, PERF_LOCK). 4셀 PASS, `dealer_router_reqrep_inproc`은 0.8817배 FAIL이며 MP-9가 귀속한 허용된 개선으로 기록만 한다. with_stream은 load 1.5 미만을 기다린다.
- 2026-09-08 03:39 KST: 다음 load check는 `0.78/1.23/0.91`로 조건을 충족했다. with_stream은 시작 load `0.51/1.13/0.89`, PERF_LOCK·local Core·reuse-build로 실행 중이며 64 B는 zlink/asio/zmq 모두 mismatch 0으로 완료했다.
- 2026-09-08 03:41 KST: with_stream 64/1024/65536 B 모두 완료(mismatch 0)했다. 결과와 staged 목록·빌드·CTest·ABI·hotpath·load를 `gate-mp-summary.md`에 기록했다. MP patch는 staged로 유지하며 commit·stash·spec 수정은 하지 않았다.
