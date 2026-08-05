# C++ Stream E2E Client Package Notes

e2e client package는 `zlink::stream_e2e_client` target을 제공한다.

- core connector package에 의존한다.
- `task_t`와 `async()` 표면은 이 package를 선택했을 때만 보인다.
- server e2e, smoke, perf scenario client에서 사용한다.
- engine wrapper package의 public API로 전파하지 않는다.
