# STREAM pull-model 변형(asio_pull, cppserver_pull) 설계와 6개 스택 비교 측정

작성 2026-09-08. 벤치 코드만 변경(Core 무변경). 측정기: with_stream 러너, CCU 1000, warmup 3 s / duration 5 s,
server-io-threads 4, client-io-threads 4, 16 core WSL2.

## 1. 왜 만들었나

zlink STREAM은 pull 모델이다. I/O 스레드가 패킷을 pipe에 넣고 애플리케이션 스레드를 깨우면,
애플리케이션 스레드가 `recv_packet` → 에코 → `send_packet` 하고, 실제 쓰기는 command로 다시 I/O 스레드가 한다.
기존 `asio`·`cppserver` 서버는 **읽기 완료 콜백(=I/O 스레드) 안에서 바로 에코**한다(스레드 홉 없음).
이 구조 차이를 분리하려고 두 스택에 zlink와 같은 홉을 넣은 변형을 추가했다.

## 2. 설계

세 요소(큐 / 깨우기 / 쓰기 반환)는 두 변형이 공유한다.
공통 큐: `bindings/c/bench/with_stream/stacks/common/stream_pull_queue.hpp`.

| 요소 | 선택 | 이유 |
|---|---|---|
| 큐 | `std::mutex` + `std::deque` MPSC, 워커는 **한 번 깨면 큐 전체를 drain** | zlink 애플리케이션 스레드의 drain 루프와 같은 배칭. 워커 스레드는 1개 — zlink의 애플리케이션 스레드가 1개이므로 구조를 맞춤 |
| 깨우기 | 기본 `condition_variable`(futex), `ZLINK_BENCH_PULL_WAKE=eventfd`로 eventfd 전환 | zlink는 signaler(eventfd write + poll/read)로 깨운다. condvar는 같은 스케줄러 wake 1회이지만 fd write/read syscall이 없어 **핸드오프 비용의 하한**을 준다(= 이 변형이 보이는 격차는 보수적인 하한). 실제 zlink와 같은 eventfd 경로도 넣어 두 방식을 측정했다(§6) |
| 쓰기 반환 | 워커가 만든 에코를 소유 I/O 스레드로 `post` — asio_pull은 세션 strand로, cppserver_pull은 세션 `io_service`로 | asio 소켓은 thread-safe가 아니므로 쓰기는 반드시 I/O 스레드에서 실행되어야 한다. 이것이 zlink의 send command 왕복과 정확히 같은 홉 |

경로 대조(그 외 wire format·프레이밍·16 KiB read chunk·sndbuf/rcvbuf/backlog/nodelay·io-threads·CLI 옵션은 원본과 동일):

```
asio            io: read → parse → write
asio_pull       io: read → chunk enqueue + wake ┃ worker: parse+echo → post ┃ io: write → 다음 read
cppserver       io: onReceived → parse → SendAsync
cppserver_pull  io: onReceived → chunk enqueue + wake ┃ worker: parse+echo → post(SendAsync) ┃ io: 전송
```

구조상 남은 차이 하나: `asio`/`asio_pull`은 쓰기 완료 뒤에 다음 읽기를 시작(세션당 half-duplex)하므로 홉이 RTT에 그대로 더해지고,
`cppserver`/`cppserver_pull`은 수신을 계속 이어가므로 홉이 파이프라인에 가려진다. 이 차이가 §5 결과를 갈라놓는다.

### 워커 스레드 경유 증명

두 변형은 종료 시 `PULL_STATS` 한 줄을 찍는다(io 스레드가 enqueue, 워커가 processed, 워커가 아닌 스레드에서 처리되면 off_worker).
확정 측정 서버 로그에서:

| 스택 | wake | enqueued | processed_by_worker | processed_off_worker | echo_posts |
|---|---|---:|---:|---:|---:|
| asio_pull (64 B) | condvar | 1,390,276 | 1,390,276 | **0** | 1,390,276 |
| cppserver_pull (64 B) | condvar | 1,679,717 | 1,679,717 | **0** | 1,679,717 |
| asio_pull (64 KiB) | condvar | 469,755 | 469,755 | **0** | 93,951 |
| cppserver_pull (64 KiB) | condvar | 255,692 | 255,692 | **0** | 218,575 |
| asio_pull (64 B) | eventfd | 1,338,984 | 1,338,984 | **0** | 1,338,984 |
| cppserver_pull (64 B) | eventfd | 1,531,536 | 1,531,536 | **0** | 1,531,536 |

모든 에코가 워커 스레드를 경유했다(off_worker = 0). 64 KiB에서 `echo_posts < enqueued`인 것은 한 프레임이 여러 chunk에 걸치기 때문이다.

## 3. 확정 측정 (CCU 1000, 3 run, idle)

결과 디렉터리 `bindings/c/bench/with_stream/results/20260908_164225`.
시작 16:42:25 load 0.58 / 0.54 / 1.44, 종료 16:53:08 load 2.67(측정 종료 직후 다른 job 재개).
run 3개의 산포가 ±3 % 이내로 좁다.

| size | zlink | asio | asio_pull | cppserver | cppserver_pull | zmq |
|---|---:|---:|---:|---:|---:|---:|
| 64 B | 272.95 | 335.45 | 271.67 | 344.75 | 324.20 | 310.04 |
| 1024 B | 245.25 | 319.74 | 260.37 | 322.03 | 304.24 | 290.05 |
| 64 KiB | 32.82 | 38.61 | 17.17 | 39.25 | 42.20 | 25.99 |

(median kops/s, 3 run)

### 비율

| 비율 | 64 B | 1024 B | 64 KiB |
|---|---:|---:|---:|
| zlink / asio | 0.814 | 0.767 | 0.850 |
| zlink / asio_pull | **1.005** | **0.942** | **1.911** |
| zlink / cppserver | 0.792 | 0.762 | 0.836 |
| zlink / cppserver_pull | 0.842 | 0.806 | 0.778 |
| asio_pull / asio | **0.810** | **0.814** | **0.445** |
| cppserver_pull / cppserver | 0.940 | 0.945 | 1.075 |
| zlink / zmq | 0.880 | 0.846 | 1.263 |

### 서버·클라이언트 CPU% (median, 프로세스 전체 = 코어 수 × 100 % 기준)

| size | 항목 | zlink | asio | asio_pull | cppserver | cppserver_pull | zmq |
|---|---|---:|---:|---:|---:|---:|---:|
| 64 B | 서버 | 360.7 | 306.7 | 342.9 | 271.1 | 336.2 | 379.7 |
| 64 B | 클라이언트 | 311.4 | 346.1 | 323.9 | 347.9 | 348.1 | 346.4 |
| 1024 B | 서버 | 361.7 | 310.1 | 352.8 | 278.6 | 350.8 | 383.1 |
| 1024 B | 클라이언트 | 308.6 | 347.6 | 329.5 | 349.5 | 349.9 | 347.2 |
| 64 KiB | 서버 | 245.0 | 336.4 | 255.8 | 296.8 | 344.9 | 212.8 |
| 64 KiB | 클라이언트 | 196.9 | 311.9 | 162.4 | 350.4 | 294.6 | 145.0 |

관찰: pull 변형은 원본 대비 서버 CPU가 30~70 pp 늘고(스레드 홉·큐·복사), 처리량은 줄거나 같다 —
같은 일을 더 많은 CPU로 한다. 클라이언트는 4 io 스레드(상한 400 %)에서 ~350 %로, 64 B/1024 B 상위 스택에서는
클라이언트가 부분적인 제약이 될 수 있다(스택 간 순위를 뒤집을 정도는 아니다: §4 스윕에서 스택별로 값이 뚜렷이 갈린다).

## 4. CCU 스윕 (64 B, 1 run씩, 16:20~16:24, 시작 load 1.19)

"세 스택 값이 우연히 같아 보인 것"이 공유 병목 때문인지 확인용.

| CCU | zlink | asio | asio_pull | cppserver | cppserver_pull | zmq |
|---|---:|---:|---:|---:|---:|---:|
| 100 | 207.18 | 287.67 | 234.70 | 289.34 | 269.90 | 215.43 |
| 1000 | 188.34 | 252.17 | 184.67 | 203.11 | 154.59 | 135.75 |
| 4000 | run_failed | 248.12 | 212.48 | 285.60 | 291.82 | 251.08 |

- CCU를 바꾸면 스택별 값이 서로 다른 방향·폭으로 움직인다 → **공유된 wake-latency 상한이 아니다**.
- CCU 100의 p50 지연: zlink 241 µs, asio 174 µs, asio_pull 213 µs, cppserver 173 µs, cppserver_pull 185 µs.
  큐잉이 없는 저부하에서도 pull 변형이 원본보다 30~40 µs 느리다 = 홉 1회의 지연 비용.
- 최초 15:37 측정(3 run)에서 64 B가 zlink 148.13 / asio_pull 148.92 / cppserver_pull 148.13으로 거의 같았던 것은
  **그 측정 창이 오염**되어 있었기 때문이다(같은 셀 run 산포 121~249 kops, 다른 job의 빌드 동시 진행).
  idle 창의 확정 측정에서는 272.95 / 271.67 / 324.20으로 갈라진다. 즉 "동일값"은 공유 병목이 아니라 잡음이었다.
  단, zlink ≈ asio_pull(0.5 % 차)은 idle에서도 재현된다 — 이것은 우연이 아니라 §7의 결론이다.
- zlink는 CCU 4000에서 run_failed(스킵). 이 조건에서의 zlink STREAM 연결 확장성은 별도 확인이 필요하다(이 job 범위 밖).

## 5. wake 방식 비교 (64 B, CCU 1000, 1 run, 결과 `20260908_165308`)

| 스택 | condvar (확정 3런 median) | eventfd (1런) | 차이 |
|---|---:|---:|---:|
| asio_pull | 271.67 | 267.60 | −1.5 % |
| cppserver_pull | 324.20 | 306.11 | −5.6 % |

zlink와 같은 eventfd(write 8 B + blocking read)로 바꿔도 1.5~5.6 %만 더 느리다.
즉 **핸드오프 비용의 대부분은 fd syscall이 아니라 스레드 홉 자체**(스케줄러 wake + 캐시 이동 + 배치 지연)다.
기본값을 condvar로 둔 것은 하한(보수적 비교)을 주기 위해서다.

## 6. 결론 — 격차의 분해

64 B / 1024 B (지연 지배 구간):

- **asio → asio_pull에서 −19 %**(0.810 / 0.814). 이것이 pull 홉의 값이다.
- **zlink / asio_pull = 1.005 / 0.942**. 즉 zlink는 같은 스레드 구조를 가진 asio와 사실상 같거나(64 B) 6 % 낮다(1024 B).
- 따라서 zlink vs asio 격차(0.814 / 0.767, 즉 −19 % / −23 %)에서 **pull 핸드오프가 −19 %p 전부(64 B) 또는 −19 %p 중 대부분(1024 B)**을 설명하고,
  **zlink 자체 계층(engine·pipe·msg·command)의 몫은 64 B에서 ≈0 %, 1024 B에서 ≈6 %**다.
- 단 이 분해에는 조건이 붙는다. `cppserver_pull / cppserver = 0.94`로, **같은 홉을 넣어도 읽기를 멈추지 않고 파이프라인화하면 손실이 6 %에 그친다**.
  즉 −19 %는 "pull 모델의 필연적 비용"이 아니라 "**pull 홉 + 세션당 read→write 직렬화**"의 합이다.
  zlink STREAM이 cppserver_pull식으로 수신을 계속 진행시킬 수 있다면(계약 범위 확인 필요) 상당 부분이 회수 가능하다는 뜻이다.
  이 기준(zlink / cppserver_pull = 0.842 / 0.806)이 zlink 자체 계층 몫의 상한이다.

64 KiB (대역 지배 구간):

- asio_pull은 원본의 0.445로 무너진다(단일 워커가 64 KiB chunk를 복사·파싱하며 병목). zlink는 asio_pull의 **1.91배**로 오히려 빠르다.
- 반면 cppserver_pull은 원본의 1.075 — 홉이 손해가 아니다.
- 따라서 64 KiB에서 zlink / asio = 0.850의 격차는 "홉" 때문이 아니라 큰 payload 경로(복사·write 병합)의 문제이며,
  zlink / cppserver_pull = 0.778이 그 몫을 보여준다. (별도 job S-D가 cppserver의 큰 payload 이득을 분석 중이다.)

정리:

| 구간 | pull 홉이 설명하는 몫 | zlink 자체 계층 몫 |
|---|---|---|
| 64 B | −19 %p (asio→asio_pull 전부) | ≈ 0 % (zlink/asio_pull 1.005), 상한 −16 % (zlink/cppserver_pull 0.842) |
| 1024 B | −19 %p | −6 % (zlink/asio_pull 0.942), 상한 −19 % (zlink/cppserver_pull 0.806) |
| 64 KiB | 없음(홉은 cppserver_pull에서 오히려 +7 %) | −15 % ~ −22 % (zlink/asio 0.850, zlink/cppserver_pull 0.778) |

## 7. 변경 파일 (커밋하지 않음, 메인 체크아웃 /home/hep7hep7/project/zlink)

신규:
- `bindings/c/bench/with_stream/stacks/common/stream_pull_queue.hpp`
- `bindings/c/bench/with_stream/stacks/asio_pull/test_scenario_stream_asio_pull.cpp`
- `bindings/c/bench/with_stream/stacks/cppserver_pull/test_scenario_stream_cppserver_pull.cpp`
- `bindings/c/bench/with_stream/stacks/cppserver/upstream/performance/stream_pull_server.cpp` (러너가 생성하는 upstream 진입점, `stream_fixed_server.cpp`와 같은 방식)

수정:
- `bindings/c/bench/with_stream/CMakeLists.txt` (`test_scenario_stream_asio_pull` 타깃)
- `bindings/c/bench/with_stream/run_benchmarks.sh` (`STACKS_ALL`·`--stack` 파싱·usage·BIN 경로·`try_build_stack`·`start_server`에 두 스택 등록)
- `bindings/c/bench/with_stream/README.md`, `README.ko.md`

측정 산출물(untracked): `results/20260908_153737`(오염 창), `20260908_162008/162121/162232`(CCU 스윕), `20260908_164225`(확정), `20260908_165308`(eventfd).

## 8. 남은 한계

- 확정 측정은 3 run이다. 15:37 측정은 다른 job의 빌드와 겹쳐 오염되어 결합하지 않았다(같은 셀에서 값이 40 % 낮고 산포가 크다).
- 클라이언트 CPU가 io 스레드 상한(400 %)의 ~87 %까지 올라간다. 상위 스택들의 절대값은 클라이언트에 눌려 있을 수 있다.
- zlink CCU 4000 64 B는 run_failed로 스킵되었다(원인 미조사).
