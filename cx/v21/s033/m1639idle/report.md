# #1639 C++ receive-role handoff — A/B 측정 상태

## 결과

A/B 수치를 산출하지 못했다. 두 벤치 모두 빌드 전이며, 셀별 throughput·CPU·worker count 표본은 없다. 따라서 submit-path 회귀의 측정 가능 여부도 판정할 수 없다.

빌드 요청은 지정된 `/root/jobs/fx/cx/cpp-build-slot.sh`로 제출했지만, 다른 작업의 빌드와 측정이 이어진 뒤 `measure.gate` 배타 잠금에서 20분 넘게 대기했다. 2026-11-11 14:53 이후 gate가 열리지 않았고, 확인 시 `measure.lock`에는 소유자나 대기자가 없었지만 gate의 배타 잠금은 남아 있었다. gate를 해제하거나 우회하지 않았다. 증거: [`blocker-state.txt`](/root/jobs/fx/cx/v21/s033/m1639idle/raw/build/blocker-state.txt), 대기된 빌드의 빈 [`base.log`](/root/jobs/fx/cx/v21/s033/m1639idle/raw/build/base.log).

## 측정 조건

승인 조건은 1024 B Framework C++ `send-saturation`과 `request-backpressure`, 각각 30초 warmup·5초 active·3회이며, 회차 안에서 base/PR 순서를 교대하는 것이다. source CPU 0–7, target CPU 8–15, CPU 0–15 governor `performance`를 `measure.lock` 안에서만 적용하고 종료 시 `powersave`로 복구할 계획이었다. **측정 잠금을 획득하지 못해 governor·고정 조건은 적용하지 않았고 측정도 시작하지 않았다.** 런별 uptime/load, free memory, CPU 0–15의 `scaling_cur_freq` 기록은 없다.

## 검증

- 빌드 명령: `/root/jobs/fx/cx/cpp-build-slot.sh /root/jobs/fx/cx/v21/s033/m1639idle/raw/build/build.sh base` (PR 빌드 미시작). 실제 명령 스크립트: [`build.sh`](/root/jobs/fx/cx/v21/s033/m1639idle/raw/build/build.sh). configure 로그: [`base.log`](/root/jobs/fx/cx/v21/s033/m1639idle/raw/build/base.log). 벤치 CMake 복사본은 [`src-base`](/root/jobs/fx/cx/v21/s033/m1639idle/src-base), [`src-pr`](/root/jobs/fx/cx/v21/s033/m1639idle/src-pr)에 있다. 원본은 각각 `/root/worktree/zlink-v21-s033/m1639-base/framework/bench/grpc/cpp`와 `/root/worktree/zlink-v21-s033/trole-pr/framework/bench/grpc/cpp`다.
- 수집기: [`run-measure.py`](/root/jobs/fx/cx/v21/s033/m1639idle/raw/run-measure.py). 예정된 런 원본 디렉터리는 `/root/jobs/fx/cx/v21/s033/m1639idle/measure/`이며 실행 자료는 생성되지 않았다.
- 실행 바이너리의 Framework 링크 경로·timestamp·SHA-256은 검증하지 못했다. 빌드가 시작되지 않아 바이너리도 없다. 두 worktree의 `git status`는 각각 detached base `fd0e665655`와 PR branch `perf/1639-cpp-receive-role-handoff`만 표시하며 tracked 변경은 없다: [`blocker-state.txt`](/root/jobs/fx/cx/v21/s033/m1639idle/raw/build/blocker-state.txt).
- 테스트는 실행하지 않았다.

리팩토링 점검: 성능 0건, POSDDD 0건, 불필요 코드 0건 발견. 고친 항목 0, 넘긴 항목 없음(코드 변경 없음).

## 남은 문제

공용 gatekeeper가 `measure.gate`를 다시 허용해야 빌드와 측정을 재개할 수 있다. 미시작 상태의 빌드 출력은 worktree에 없으며 두 worktree는 유지했다. 현재 수치 기반 결론은 없다.
