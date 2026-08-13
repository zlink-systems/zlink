# C Binding Benchmark Guidelines

- Core runtime 기준 경로는 `core/build`이다. 다른 임시 build 결과로 benchmark를 판단하지 않는다.
- `core/src/` 또는 `core/include/` 변경 후 `cmake --build core/build`로 runtime을 다시 만든다.
- 실행 전에 runner가 실제 `libzlink.so` 경로를 출력해야 한다.
- Runtime이 source보다 오래됐으면 benchmark를 중단한다.
- 같은 규칙을 root `README`와 `bindings/c/perf/README.md`에서 일관되게 유지한다.
