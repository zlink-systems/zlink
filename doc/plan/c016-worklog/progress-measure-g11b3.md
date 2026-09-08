# measure-g11b3 진행

- 2026-09-07 KST: 작업 시작. `main` 확인, staged G-11b-3 3개 runtime 파일과 `core/doc/spec/**`의 감독관 미커밋 변경을 읽기 전용으로 확인했다. 공통 규칙·GATE 절차 6~8·with_stream README를 읽었고, 다음으로 CPU idle 확인을 수행한다.
- 18:03 KST: `pgrep -x ninja`는 세 번 모두 비었고, 1분 load average는 0.18 → 0.37로 1.0 미만을 유지했다. Release lib-only 빌드를 시작한다.
- 18:04 KST: `JOBS=4 scripts/build-core.sh release --lib-only`가 `core/build`의 `libzlink.so`를 생성하고 완료했다. 직후 load 1분값은 2.86이므로, 측정 전 1.0 미만 2분 안정 조건을 대기 중이다.
- 18:07 KST: build 이후 load 1분값은 0.69 → 0.35로 낮아졌다. pristine 결과 디렉터리는 현재 트리에 없지만, 기존 G-11b-3 요약의 3회 중앙값(64/1024/64 KiB 비율 0.812322/0.813308/0.819557)을 대조 값으로 확인했다. idle 2분 조건을 마저 확인한다.
- 18:10 KST: 지정 `PERF_LOCK`을 획득한 with_stream runs=3가 진행 중이다. 결과 경로는 `bindings/c/bench/with_stream/results/G-11b3-after-measure-20260907_180800/`; 1분 load는 측정 부하 중 3.04이며 raw metrics 23행이 기록되었다. 오류/skip은 아직 보이지 않는다.
- 18:13 KST: with_stream은 마지막 64 KiB run 묶음을 진행 중이며 metrics 49행을 기록했다. 현재 측정 중 load는 2.16/1.87/1.34이고, runner/lock은 계속 살아 있다.
- 18:15 KST: with_stream 27/27 PASS 및 3회 중앙값 집계를 완료했다. pristine 대비 zlink/asio 변화는 +0.206%/+0.513%/+0.041%로 모두 −5% 이내다. 이어서 같은 lock 아래 perf/c 1024 B tcp 경량 3셀을 1회 완료했고, 요약 보고서를 작성했다.
