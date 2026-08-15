# Java Router request zero-copy 후보 결과

## 결과

Router request part가 가진 native `msg_t`를 임시 native message에 복사하지 않고
Core에 직접 전달하는 후보를 적용했다. 64B부터 4KiB까지 throughput은 올랐지만 64KiB
client가 timeout되어 run이 `partial`로 종료했다.

Router request의 실패 경로는 public `Message` ownership을 보존하기 위해 기존 복사
경로를 사용한다. complete 결과가 아니므로 후보를 원복했고, 결과표에는 이전 complete
측정값만 사용한다.

- Core: release `0.10.1`
- 대상: `MULTI_ROUTER_ROUTER_REQREP / tcp`, clients `100`, duration `2초`, runs `1`
- C report: `/tmp/zlink-java-router-request-c/multi/report/perf_c_multi_linux_20260813_215941_java-router-request-c.txt`
- Java partial report: `/tmp/zlink-java-router-request-java/multi/report/perf_java_multi_linux_20260813_220001_java-router-request-java.txt`
- 검증: `CallbackSendContractTest`, `RequestResultMappingContractTest`, `RequestReplyTerminationContractTest` 통과
