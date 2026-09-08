# attrib-0172 진행

- 2026-09-08 KST: 시작. `_common-rules.md`, `core-rf-GATE.prompt` 절차 6·8, `doc/AGENTS.md`, `bindings/c/perf/README.md`와 `AGENTS.md`를 확인했다. `main`의 `core/build/lib/libzlink.so.0.17.2`는 03:57:35 KST 갱신본이다. 기존 untracked worklog는 보존한다.
- 2026-09-08 KST: base `5304885197` detached worktree를 `/home/hep7hep7/project/zlink-work/base-5304885197`에 만들고 `JOBS=4 scripts/build-core.sh release --lib-only`를 성공했다. `libzlink.so.0.17.1`은 04:54:37 KST 갱신됐다.
- runner는 main `bindings/c/build`의 동일 binary를 유지한다. binary는 main lib RUNPATH를 가지지만 `LD_LIBRARY_PATH`가 우선이며, multi runner의 `ZLINK_PERF_SERVER_RUNTIME_DIR`/`ZLINK_PERF_CLIENT_RUNTIME_DIR` override가 이를 두 프로세스 모두에 prepend한다. base override `ldd`와 `LD_DEBUG=libs`에서 실제 `.../base-5304885197/core/build/lib/libzlink.so.0` 로드를 확인했다.
- 2026-09-08 KST: single runner는 multi와 달리 override를 적용하지 않고 main runtime을 prepend한다. 첫 base 시도는 실제 main을 로드했음을 확인해 즉시 중단·폐기했다. 같은 fixed binary에 `LD_PRELOAD=<정확한 libzlink.so>`를 사용하기로 전환했다. base와 0.17.2 각각 `ldd`와 `LD_DEBUG=libs`의 `calling init`가 지정 versioned library를 실제 로드함을 확인했다.
- 04:59 KST 재측정도 시작 load 3.79로 idle 조건 위반이라 첫 셀 중 즉시 중단·폐기했다. 확인 결과 다른 job의 `zlink-work/st1` dev build(`cc1plus` 4개)가 실행 중이며 ninja는 0(Unix Makefiles)이다. PERF_LOCK은 사용 가능하지만 build 부하가 있으므로 유휴가 될 때까지 측정을 보류한다.
- 05:03:30 KST에 외부 build 종료를 확인했다. 05:05:08~05:07:18에 1분 load 0.71→0.60, ninja·compiler 0의 2분 유휴 조건을 확보했다.
- 05:07:39~05:12:30 KST: PERF_LOCK 아래 valid single 교대(base1→current1→base2→current2)를 완료했다. 모두 4/4 성공, 시작 load는 0.47 / 1.39 / 1.46 / 1.67(후속 세 값은 바로 앞 bench의 잔류 load)이고 ninja는 모두 0이다. 결과·loader trace는 scratchpad와 `bindings/c/perf/results`에 저장했다. base/current 첫 교대의 PAIR 1024 B 중앙값은 799.4/749.0 Kmsg/s, 두 번째는 761.2/733.7 Kmsg/s다.
- 05:13:18~05:15:27 KST: multi 전에도 load <1.0과 ninja·compiler 0의 2분 유휴 조건을 확보했다. multi shell runner는 의도적 runtime 교체를 RUNPATH identity 검증에서 거부하므로, 같은 `bindings/c/build` executable을 호출하는 기존 `run_comparison.py` backend로 실행했다. `LD_PRELOAD`와 `LD_DEBUG=libs` trace는 모든 교대에서 지정 lib만 로드했음을 확인한다.
- 05:16:44~05:19:52 KST: valid multi 교대(base1→current1→base2→current2)를 완료했다. 모두 2/2 성공, 시작 load는 0.15 / 3.60 / 4.03 / 3.96이고 ninja는 모두 0이다. DR_REQREP 비율은 79.70%/98.35%, RR_SENDSEND는 94.69%/97.21%로, 두 교대 모두 5% 이상 하락한 셀이 없다.
- 완료: 요약 `attrib-0.17.2-summary.md`를 작성했다. 최종 판정은 **기준 불일치(§7.4 Phase 2G 값의 측정 조건 차이)**이며 MP 계열 회귀 후보는 없다. base worktree는 남겼다.
