2026-09-05 시작: detached @296c5c04e5 확인, 기존 untracked core/build-main-readonly 보존, 허용 범위 내 조사 착수.
2026-09-05 기준 준비: 누락된 core/build를 기존 read-only release build symlink로 연결; runtime=/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.
2026-09-05 기준 재현 첫 시도: --reuse-build 대상 bindings/c/build 부재로 즉시 종료(exit 1); Core는 건드리지 않고 C perf build를 먼저 생성하기로 함.
2026-09-05 C perf 최초 build+기준 측정 완료(exit 0): ws 1024/4096/8192=188056/143761/104204 ops/s (mean 1.535/24.674/27.982 ms), wss=154875/3338/16898 (4.511/28.025/66.183 ms), tcp=173018/115928/96709 (0.895/1.292/2.918 ms); report=bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_045753.txt.
2026-09-05 지정 --reuse-build 기준 재현 완료(exit 0): ws=172366/138564/89959 ops/s (1.301/8.870/26.380 ms), wss=137300/4397/12301 (4.032/20.658/55.764 ms), tcp=169424/125686/92416 (0.879/1.472/2.733 ms); wss 4096B 붕괴 재현, report=bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_045900.txt.
