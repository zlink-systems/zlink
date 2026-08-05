# C++ Stream Connector Performance Client

`connector_perf_client`는 많은 일반 client가 connector를 통해 request/wait 흐름을 수행하는지
검증하기 위한 tool이다. server orchestration은 runner script나 CTest fixture가 담당하고,
perf executable은 client 부하 생성과 report 작성만 담당한다.

## Smoke

```bash
cmake --build framework/languages/cpp/build --target connector_perf_smoke
ctest --test-dir framework/languages/cpp/build -L connector-perf-smoke --output-on-failure
```

smoke는 10 clients, 2 workers, 5s 설정으로 CI에서 loopback request/wait coroutine 흐름과 report
schema를 확인한다. 외부 server endpoint를 요구하지 않으며 `mode`는 `loopback-request-wait`로
기록된다.

`--workers`는 report에만 쓰는 값이 아니다. perf client는 connector를 만들기 전에 shared connector
runtime의 worker thread 수를 이 값으로 설정한다.

## Scale

```bash
framework/languages/cpp/build/connector_perf_client \
  --clients 5000 \
  --workers 4 \
  --duration 60s \
  --warmup 10s \
  --request-timeout-ms 1000 \
  --transport tcp \
  --dispatch-mode immediate \
  --endpoint tcp://127.0.0.1:7000 \
  --report framework/languages/cpp/build/connector-perf-5000.json
```

scale 실행은 실제 STREAM test server endpoint가 필요하다. endpoint를 넘기면 perf client는 지정된
client 수만큼 connector를 만들고 e2e client coroutine request 흐름을 실행한다. 일부 client는
server push packet을 `wait_for().async()`로 기다린다. loopback smoke는 같은 흐름을 작은 client 수로
검증한다. report에는 실행 시간, request 처리량, p50/p95/p99 latency, wait 완료 수, timeout 수,
오류 수가 기록된다.

scale 결과는 hardware 차이가 크므로 절대 수치만으로 실패시키지 않는다. thread 수가 client 수에
비례해 증가하거나, timeout/error 비율이 threshold를 넘거나, RSS가 비정상적으로 증가하면 회귀로
판단한다.
