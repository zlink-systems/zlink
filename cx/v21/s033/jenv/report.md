| 항목 | main Framework | prototype paced | binding paced |
|---|---|---|---|
| request / reply record B | 1,435 / 1,443 | 1,092 / 1,092 | 1,252 / 1,252 |
| source IO/0 CPU µs/request | 12.892 | 10.943 | 7.620 |
| source IO/2 CPU µs/request | 12.415 | 12.070 | 4.892 |
| target IO/0 CPU µs/request | 10.004 | 8.592 | 6.047 |
| target IO/1 CPU µs/request | 13.554 | 12.767 | 6.201 |
| source I/O 합계 CPU µs/request | 25.317 | 23.023 | 12.520 |
| target I/O 합계 CPU µs/request | 23.568 | 21.368 | 12.254 |
| source mesh CPU µs/request | 12.371 | 11.047 | 해당 thread 없음 |
| target mesh CPU µs/request | 10.699 | 9.727 | 해당 thread 없음 |
| caller µs | 12.642 | 10.679 | 8.778 |
| active 전체 req/s | 61,132.050 | 61,129.150 | 61,131.750 |
| perf 창 req/s | 58,498.153 | 61,166.253 | 61,131.136 |
| BPF 창 req/s | 59,073.091 | 61,002.205 | 52,154.737 |

단위는 완료 요청 하나당 CPU 시간이다. 첫 표의 prototype과 binding은 main의 전체 처리율을 반올림한 61,132 req/s로 pacing했다. Source는 CPU 0–7, target은 8–15에 고정한 condition B다. request-backpressure, payload 1,024 B, warmup 30초, active 60초, 최종 유효 실행은 조건마다 1회다. 각 실행은 `/root/jobs/fx/cx/measure.lock`의 배타 lock 안에서 진행했다. MemAvailable 최솟값은 24.67 GiB다.

Prototype unpaced 처리율은 68,907.867 req/s이고 caller는 9.409 µs다. Source/target mesh CPU는 10.350/8.313 µs/request다. Main 대비 처리율은 1.127배다. 각 조건의 완료 수는 main 3,668,336, prototype unpaced 4,134,954, prototype paced 3,667,896, binding paced 3,667,909건이다. 모두 submitted = completed = target received이고 source errors, target errors, abandoned는 0이다.

첫 표의 CPU는 active 안의 약 10초 schedstat delta를 같은 창의 완료 수로 나눈 값이다. 전체 처리율과 perf/BPF 창 처리율을 별도로 적었다. 평균 pacing이 같아도 수집 창과 도착 간격은 완전히 같지 않으므로 작은 차이는 정밀한 인과 효과로 해석하지 않는다. Caller는 각 `results.json`의 `jvm_thread_cores × 1e6 / throughput_per_second`다.

201 B는 비교 bench가 만드는 JSON application part다. 생성 위치는 `framework/bench/grpc/java/shared/src/main/java/systems/zlink/bench/withgrpc/shared/RawWire.java:23`의 `REQUEST_ENVELOPE`와 `:29`의 `RESPONSE_ENVELOPE`다. Request의 kind만 1이고 reply는 2다. `channelName=bench`, `messageName=BenchPayload`, `contentType=application/x-protobuf`이며 correlationId, deadline, topic, errorCode, errorMessage, source는 null이다. Capture에서도 두 방향의 native parts가 `[201, 1027] B`이고 framing 24 B를 더한 record가 1,252 B였다.

이 bench는 `bindings/java/perf` harness를 사용하지 않는다. `RawStack.java:69–77`에서 public `RouterSocket.request(peer).message(header).message(body).timeout(timeout).submit()`을 직접 호출한다. Server는 `ZLinkRawBenchServer.java:115–123`에서 `RESPONSE_ENVELOPE`를 만들고 원래 `received.reply()`에 두 part를 전달한다. Binding의 `NativeRouterSocket.java:82`는 받은 parts를 request builder에 전달하며, `CompletionOwner.java:605–654`는 native completion token으로 요청을 찾는다. 201 B JSON을 자동으로 삽입하는 코드가 아니다. 실제 dependency는 Java binding 1.23.0 package다.

Native part와 record 구성은 다음과 같다. Application payload 1,027 B는 세 조건에서 같은 protobuf BenchPayload다. 측정용 1,024 B body와 protobuf tag/length 3 B를 포함한다.

| 실행 | request native parts B | reply native parts B | 추가 구성 |
|---|---|---|---|
| main | 13 + 1398 | 21 + 1398 | 1398 = wrapper 75 + JSON header 296 + payload 1027 |
| prototype | 41 + 1027 | 41 + 1027 | descriptor 41, wrapper와 JSON header 없음 |
| binding | 201 + 1027 | 201 + 1027 | bench JSON header 201 |

각 capture에서 request와 reply 하나씩 해석한 결과는 다음과 같다. 두 Framework 조건은 같은 요청의 identity가 reply에 보존됐다. 상세 header와 hex는 `decoded-examples.json`과 각 `wire.json`에 있다.

| 실행 | 방향 | operation 또는 kind | channel | message | codec 또는 content type | identity / correlationId | deadline |
|---|---|---|---|---|---|---|---|
| main Framework | request | 1 | bench | BenchPayload | application/x-protobuf | 40567e06c238cc790000000000000002 | 없음 |
| main Framework | reply | 2 | bench | BenchPayload | application/x-protobuf | 40567e06c238cc790000000000000002 | 없음 |
| prototype paced | request | 17 | bench | BenchPayload | 1 | 3313b7cc89f2d8eb0000000000000002 | 없음 |
| prototype paced | reply | 20 | bench | BenchPayload | 1 | 3313b7cc89f2d8eb0000000000000002 | 없음 |
| binding paced | request | 1 | bench | BenchPayload | application/x-protobuf | null | 없음 |
| binding paced | reply | 2 | bench | BenchPayload | application/x-protobuf | null | 없음 |

각 방향의 ZMP framing은 모두 24 B이고 native part 수는 모두 2개다. Prototype descriptor는 magic 2 B, major 1 B, operation 1 B, flags 1 B, codec 1 B, identity 16 B, channel text8 6 B, message text8 13 B다. Node request command는 생성 상수와 실제 capture에서 17이고 Channel request는 19, reply는 20이다. Request 종류를 별도 kind byte로 반복하지 않는다. 이 bench는 deadline을 wire에 넣지 않으며 binding timeout 30초를 유지한다. Descriptor의 선택 deadline 표현은 epoch milliseconds 8 B다. 이 프로토타입은 빈 metadata와 flow off만 허용하며 해당 필드를 생략한다. 일반 Node/Channel request는 major 2만 받으며 JSON 해석 실패 후 다른 형식을 시도하는 fallback은 없다. Control traffic의 기존 major 1 처리는 유지한다.

높음으로 분류했던 TCP 가설과 socket 옵션은 아래 계측으로 판단한다.

| design.md 4절의 가설 | 관측 | 판정 |
|---|---|---|
| 높음: record 크기와 write batch 충전률 | Prototype의 request/reply는 main보다 23.9%/24.3% 작다. 50k에서 source send/receive와 target receive의 TCP cycles/request는 각각 22.4%, 22.3%, 19.4% 감소했다. 반면 target send는 20,174 → 20,695 cycles/request로 줄지 않았다. Reply의 성공 write당 record 수는 main 1.36, prototype 1.34, binding 1.64다. | 바이트 감소의 기여는 지지한다. Prototype reply의 호출/request는 binding의 약 1.22배지만 TCP cycles/request는 약 2.62배이므로 write 묶음 차이만으로 나머지를 설명하지 못한다. Request 방향은 Framework의 write당 record 수가 binding보다 많아서 Framework가 모든 방향에서 batch를 덜 채운다는 설명은 지지하지 않는다. |
| 높음: native frame 크기 분포와 receive zero-copy 성공률 | 기본 실행에서 main의 1,398 B payload part가 allocator 범위에 들어가지 못한 비율은 target/source 14.08%/16.89%다. Prototype의 1,027 B는 2.64%/3.61%, binding은 0.16%/0.32%다. Main의 13/21 B header는 inline type 101, prototype의 41 B와 binding의 201 B header는 type 105다. Native part 수는 모두 2개다. | Frame 크기와 수신 경계에 따른 decoder 경로 차이는 확인했다. 다만 이 계측은 기본 61k 실행이며 BPF 창 rate가 달라 동일 부하에서의 단독 인과 검사는 아니다. 50k에서 prototype source receive의 Core userspace는 1.513 µs, binding은 1.022 µs지만 TCP receive는 7.152/2.602 µs다. 직접적인 decoder 비용만으로 이 kernel 차이를 설명하지 못한다. Allocation 수·copy bytes·잔여 read buffer 길이는 직접 계수하지 않았다. |
| 높음: payload 배치가 send copy와 buffer 경계에 미치는 영향 | 50k에서 userspace memcpy/memmove의 self 표본은 main source/target 0.176/0.045 µs, prototype 0.091/0.044 µs, binding 0.179/0.045 µs다. 각 process의 해당 표본은 1–4개이므로 정밀한 비용 추정은 아니다. 모든 application part는 기본 gather threshold 65,536 B보다 작다. | 직접적인 userspace copy 비용만으로 큰 I/O 차이를 설명하는 가설은 지지하지 않는다. 50k prototype의 target send TCP cycles/B는 18.952로 binding 6.317보다 높고, source receive도 12.264/5.252다. 이 값은 userspace copy를 제외한 kernel 계수 추정이다. Copy·allocator가 cache나 kernel buffer 상태에 미치는 간접 효과는 배제하지 못했다. |
| TCP_NODELAY와 socket buffer 차이 | 실제 Application/Completion connection의 양 끝 모두 TCP_NODELAY=1이다. 50k의 SO_SNDBUF는 모두 4,194,304 B다. sk_userlocks는 source 0, target 4로 send/receive buffer 고정 bit 1/2가 없다. Target의 4는 bind-address lock이다. Prototype source의 수신 buffer는 3,086,259 B였고 main/binding은 6,291,456 B였다. | TCP_NODELAY 설정 차이는 배제한다. 서로 다른 고정 socket buffer 설정의 증거도 없다. 실제 자동 조정된 수신 buffer 크기는 다르므로 buffer 상태의 영향까지 배제하지 않는다. MSS·RTT·재전송·GRO/SKB 경계는 이번에 직접 계수하지 않았다. |

바이트 감소와 수신 frame 경로 변화는 관측됐지만 I/O 차이를 전부 설명하지 못했다. 특히 같은 50k에서 prototype의 reply send TCP cycles/request는 binding의 약 2.62배다. 남은 차이를 Framework wrapper 해석이나 registry 조회 시간으로 합산하지 않는다. 해당 해석은 mesh thread에서 제거한 작업이며 kernel TCP 비용과 별개다. Sync/async 완료 비율과 partial-write 비율도 직접 분리하지 않았으므로 어느 buffer 경계가 최종 원인인지는 확정하지 않는다.

세 조건의 Java binding jar SHA-256은 `077dc5df4c1a473365e03abe5605846ab8e7620d952bf48ec6bc41f976d9d714`로 같다. 로드한 native library는 `/root/.cache/zlink/native/6f496304ef6e1287f57234567231bbd2b76ad2392f99e816001417d01215bf01/libzlink.so`이며 SHA-256도 경로의 값과 같다. `ZLINK_ASIO_GATHER_THRESHOLD`는 설정하지 않았다. 소스의 기본 threshold와 관측한 part 크기로는 1,027/1,398 B 사이에서 gather threshold를 넘었다는 설명을 지지하지 않는다. 실제 gather 분기 횟수는 계수하지 않았다.


아래 I/O 분해는 cpu-clock 49 Hz 표본의 stack 분류 비율을 각 thread의 schedstat CPU에 적용한 추정이다. TCP send/receive는 sendto/recvfrom과 그 하위 stack을 포함한다. Epoll은 wait/ctl, eventfd/wake는 read/write 경로다. Core userspace는 syscall 범주 밖의 I/O thread 표본이며 allocator와 memcpy도 포함한다. 기타 syscall은 별도 열에 남겼다. 한 표본을 두 범주에 넣지 않았다. CPU delta가 있으나 cpu-clock 표본이 없는 idle I/O thread는 미표본 열에 남겼다.

| 실행 | thread | 합계 µs | TCP send | TCP receive | epoll | eventfd/wake | Core userspace | 기타 syscall | 미표본 | 표본 수 |
|---|---|---|---|---|---|---|---|---|---|---|
| main Framework | source IO/0 | 12.892 | 9.246 | 0.000 | 1.316 | 0.000 | 2.293 | 0.038 | 0.000 | 343 |
| main Framework | source IO/1 | 0.005 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.005 | 0 |
| main Framework | source IO/2 | 12.415 | 0.000 | 7.769 | 2.361 | 0.038 | 1.942 | 0.305 | 0.000 | 326 |
| main Framework | source IO/3 | 0.005 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.005 | 0 |
| main Framework | target IO/0 | 10.004 | 0.000 | 5.820 | 1.788 | 0.342 | 1.902 | 0.152 | 0.000 | 263 |
| main Framework | target IO/1 | 13.554 | 10.119 | 0.000 | 1.232 | 0.000 | 2.166 | 0.037 | 0.000 | 363 |
| main Framework | target IO/2 | 0.005 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.005 | 0 |
| main Framework | target IO/3 | 0.005 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.005 | 0 |
| prototype unpaced | source IO/0 | 9.826 | 7.254 | 0.000 | 1.088 | 0.000 | 1.418 | 0.066 | 0.000 | 298 |
| prototype unpaced | source IO/1 | 0.005 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.005 | 0 |
| prototype unpaced | source IO/2 | 11.011 | 0.000 | 6.640 | 1.935 | 0.234 | 1.668 | 0.534 | 0.000 | 330 |
| prototype unpaced | source IO/3 | 0.004 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.004 | 0 |
| prototype unpaced | target IO/0 | 7.734 | 0.000 | 4.520 | 1.339 | 0.402 | 1.306 | 0.167 | 0.000 | 231 |
| prototype unpaced | target IO/1 | 11.969 | 9.091 | 0.000 | 1.308 | 0.000 | 1.504 | 0.065 | 0.000 | 366 |
| prototype unpaced | target IO/2 | 0.004 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.004 | 0 |
| prototype unpaced | target IO/3 | 0.004 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.004 | 0 |
| prototype paced | source IO/0 | 10.943 | 8.153 | 0.000 | 1.087 | 0.000 | 1.667 | 0.036 | 0.000 | 302 |
| prototype paced | source IO/1 | 0.005 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.005 | 0 |
| prototype paced | source IO/2 | 12.070 | 0.000 | 6.971 | 2.165 | 0.404 | 2.091 | 0.440 | 0.000 | 329 |
| prototype paced | source IO/3 | 0.005 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.005 | 0 |
| prototype paced | target IO/0 | 8.592 | 0.000 | 5.089 | 1.549 | 0.295 | 1.475 | 0.184 | 0.000 | 233 |
| prototype paced | target IO/1 | 12.767 | 9.512 | 0.000 | 1.194 | 0.000 | 2.062 | 0.000 | 0.000 | 353 |
| prototype paced | target IO/2 | 0.004 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.004 | 0 |
| prototype paced | target IO/3 | 0.005 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.005 | 0 |
| binding paced | source IO/0 | 7.620 | 5.334 | 0.000 | 0.617 | 0.000 | 1.597 | 0.073 | 0.000 | 210 |
| binding paced | source IO/1 | 0.003 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.003 | 0 |
| binding paced | source IO/2 | 4.892 | 0.000 | 3.072 | 0.796 | 0.303 | 0.645 | 0.076 | 0.000 | 129 |
| binding paced | source IO/3 | 0.004 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.004 | 0 |
| binding paced | target IO/0 | 6.047 | 0.000 | 3.591 | 1.136 | 0.000 | 0.989 | 0.330 | 0.000 | 165 |
| binding paced | target IO/1 | 6.201 | 4.557 | 0.000 | 0.747 | 0.000 | 0.859 | 0.037 | 0.000 | 166 |
| binding paced | target IO/2 | 0.003 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.003 | 0 |
| binding paced | target IO/3 | 0.004 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.004 | 0 |

기본 실행의 BPF 계수는 다음과 같다. 계수 창 길이는 counter_stats의 전·후 통계 조회 블록 중간 시각 차이를 썼다. Process snapshot에 걸린 시간은 창 길이에 넣지 않았다. 성공 호출의 log2 byte histogram을 합친 분포다. HTTP 관리 socket은 제외했다.

| 실행 | thread | syscall | 계수 창 req/s | 호출/request | 성공 B/호출 | EAGAIN/request | <2 / 2–4 / 4–8 / ≥8 KiB 비율 |
|---|---|---|---|---|---|---|---|
| main Framework | source IO/0 | sendto | 59,073.091 | 0.507 | 2,793.754 | 0.000 | 48.2%/25.6%/24.7%/1.5% |
| main Framework | source IO/2 | recvfrom | 59,073.091 | 0.700 | 2,728.512 | 0.178 | 50.3%/26.3%/20.7%/2.7% |
| main Framework | target IO/0 | recvfrom | 59,073.091 | 0.721 | 2,917.964 | 0.235 | 45.8%/25.6%/25.3%/3.3% |
| main Framework | target IO/1 | sendto | 59,073.091 | 0.588 | 2,424.511 | 0.000 | 56.0%/25.1%/18.8%/0.2% |
| prototype unpaced | source IO/0 | sendto | 68,742.092 | 0.455 | 2,378.580 | 0.000 | 49.1%/35.8%/12.9%/2.2% |
| prototype unpaced | source IO/2 | recvfrom | 68,742.092 | 0.730 | 2,064.678 | 0.206 | 51.0%/40.6%/8.3%/0.1% |
| prototype unpaced | target IO/0 | recvfrom | 68,742.092 | 0.588 | 2,549.942 | 0.164 | 43.6%/37.5%/15.9%/3.0% |
| prototype unpaced | target IO/1 | sendto | 68,742.092 | 0.563 | 1,921.761 | 0.000 | 56.1%/37.5%/6.4%/0.0% |
| prototype paced | source IO/0 | sendto | 61,002.205 | 0.548 | 1,973.283 | 0.000 | 54.7%/37.0%/7.9%/0.4% |
| prototype paced | source IO/2 | recvfrom | 61,002.205 | 0.795 | 1,907.453 | 0.228 | 55.7%/37.7%/6.5%/0.1% |
| prototype paced | target IO/0 | recvfrom | 61,002.205 | 0.725 | 2,136.706 | 0.219 | 49.4%/39.5%/10.3%/0.8% |
| prototype paced | target IO/1 | sendto | 61,002.205 | 0.614 | 1,761.212 | 0.000 | 61.3%/34.1%/4.6%/0.0% |
| binding paced | source IO/0 | sendto | 52,154.737 | 0.929 | 1,322.737 | 0.000 | 94.5%/5.5%/0.0%/0.0% |
| binding paced | source IO/2 | recvfrom | 52,154.737 | 1.049 | 1,596.614 | 0.280 | 77.2%/22.1%/0.5%/0.2% |
| binding paced | target IO/0 | recvfrom | 52,154.737 | 1.109 | 1,404.325 | 0.234 | 88.6%/11.3%/0.1%/0.0% |
| binding paced | target IO/1 | sendto | 52,154.737 | 0.791 | 1,553.738 | 0.000 | 79.9%/19.6%/0.5%/0.0% |

전체 process BPF 추적은 binding의 계수 창을 약 52.2k req/s로 낮췄다. I/O tid의 TCP syscall만 추적한 61,132 req/s 재계수에서는 prototype/binding이 약 61k였으나 main은 약 56.5k로 내려갔다. 이 차이를 동일 rate 비교로 간주하지 않았다. TCP 원인 확인용으로 세 조건 모두 여유가 있는 50,000 req/s에서 warmup 30초, active 60초, 각각 1회 추가 계측했다. 아래 50k 표에는 해당 실행의 전체·perf·BPF rate를 모두 적었다.

추가 계측의 main은 보존한 main runtime과 target을 그대로 쓰고 source의 client.jar만 Framework pacing이 추가된 bench driver로 바꾼 별도 main-counter-build 사본을 사용했다. 원래 main-build 사본과 첫 표의 기본 성능 측정은 바꾸지 않았다. 50k 확인에서는 JFR을 켜지 않고 I/O/mesh thread 대상 perf와 I/O tid 대상 TCP BPF를 썼다. Framework/runtime·binding/native library는 기본 실행과 같다.

| TCP 원인 확인: 50k req/s | main Framework | prototype paced | binding paced |
|---|---|---|---|
| 전체 req/s | 49,999.600 | 49,999.717 | 49,999.733 |
| perf 창 req/s | 50,069.744 | 49,999.907 | 49,999.724 |
| BPF 창 req/s | 50,008.405 | 50,025.274 | 50,022.120 |
| source IO/0 CPU µs/request | 14.973 | 11.432 | 7.626 |
| source IO/0 TCP cycles/B | 17.060 | 17.406 | 12.825 |
| source IO/0 TCP cycles/request | 24,481.739 | 19,007.215 | 16,057.046 |
| source IO/2 CPU µs/request | 13.953 | 12.745 | 4.971 |
| source IO/2 TCP cycles/B | 11.938 | 12.264 | 5.252 |
| source IO/2 TCP cycles/request | 17,225.935 | 13,392.050 | 6,575.894 |
| target IO/0 CPU µs/request | 11.101 | 8.626 | 5.993 |
| target IO/0 TCP cycles/B | 8.430 | 8.923 | 9.327 |
| target IO/0 TCP cycles/request | 12,096.560 | 9,743.916 | 11,677.775 |
| target IO/1 CPU µs/request | 14.342 | 12.562 | 5.908 |
| target IO/1 TCP cycles/B | 13.981 | 18.952 | 6.317 |
| target IO/1 TCP cycles/request | 20,174.325 | 20,695.282 | 7,909.504 |

| 실행 | thread | syscall | 계수 창 req/s | 호출/request | 성공 B/호출 | EAGAIN/request | <2 / 2–4 / 4–8 / ≥8 KiB 비율 |
|---|---|---|---|---|---|---|---|
| main Framework 50k | source IO/0 | sendto | 50,008.405 | 0.662 | 2,135.721 | 0.000 | 65.4%/24.3%/10.0%/0.3% |
| main Framework 50k | source IO/2 | recvfrom | 50,008.405 | 0.911 | 2,233.080 | 0.274 | 64.3%/23.8%/11.2%/0.8% |
| main Framework 50k | target IO/0 | recvfrom | 50,008.405 | 1.004 | 2,251.810 | 0.377 | 62.1%/25.3%/11.7%/0.9% |
| main Framework 50k | target IO/1 | sendto | 50,008.405 | 0.723 | 1,968.141 | 0.000 | 71.4%/22.3%/6.3%/0.1% |
| prototype paced 50k | source IO/0 | sendto | 50,025.274 | 0.667 | 1,613.867 | 0.000 | 64.1%/34.1%/1.8%/0.0% |
| prototype paced 50k | source IO/2 | recvfrom | 50,025.274 | 1.009 | 1,582.644 | 0.329 | 66.7%/31.5%/1.8%/0.0% |
| prototype paced 50k | target IO/0 | recvfrom | 50,025.274 | 1.017 | 1,689.132 | 0.380 | 60.2%/37.3%/2.4%/0.1% |
| prototype paced 50k | target IO/1 | sendto | 50,025.274 | 0.735 | 1,464.377 | 0.000 | 73.5%/25.8%/0.7%/0.0% |
| binding paced 50k | source IO/0 | sendto | 50,022.120 | 0.730 | 1,691.273 | 0.000 | 70.4%/29.4%/0.2%/0.0% |
| binding paced 50k | source IO/2 | recvfrom | 50,022.120 | 0.917 | 2,107.224 | 0.331 | 51.7%/45.7%/2.5%/0.1% |
| binding paced 50k | target IO/0 | recvfrom | 50,022.120 | 0.990 | 1,763.874 | 0.290 | 68.2%/31.3%/0.5%/0.0% |
| binding paced 50k | target IO/1 | sendto | 50,022.120 | 0.603 | 2,047.552 | 0.000 | 54.3%/43.8%/1.9%/0.0% |

Socket 옵션은 해당 프로세스의 tcp_sendmsg/tcp_recvmsg 진입에서 실제 kernel sock을 읽었다. SO_SNDBUF/SO_RCVBUF는 kernel이 보유하는 값이며 userspace가 설정한 원래 인자와 구분한다. 수신 buffer는 관찰 구간에 자동 조정될 수 있어 범위를 적었다. 기본 61k 실행과 50k TCP 확인 실행을 구분해 두 값을 모두 보존했다. sk_userlocks도 함께 기록했다.

| 실행 | 계측 | process | local → remote port | TCP_NODELAY | SO_SNDBUF B | SO_RCVBUF 관측 범위 B | sk_userlocks |
|---|---|---|---|---|---|---|---|
| main Framework | 기본 | target | 5254 → 32896 | 1 | 3677184 | 6291456–6291456 | 4 |
| main Framework | 기본 | target | 5254 → 32904 | 1 | 3677184 | 131072–131072 | 4 |
| main Framework | 기본 | source | 32896 → 5254 | 1 | 3677184 | 131072–131072 | 0 |
| main Framework | 기본 | source | 32904 → 5254 | 1 | 3677184 | 6291456–6291456 | 0 |
| main Framework | 50k I/O 전용 | target | 5254 → 45644 | 1 | 4194304 | 6291456–6291456 | 4 |
| main Framework | 50k I/O 전용 | target | 5254 → 45658 | 1 | 4194304 | 131072–131072 | 4 |
| main Framework | 50k I/O 전용 | source | 45644 → 5254 | 1 | 4194304 | 131072–131072 | 0 |
| main Framework | 50k I/O 전용 | source | 45658 → 5254 | 1 | 4194304 | 6291456–6291456 | 0 |
| prototype paced | 기본 | target | 5254 → 53028 | 1 | 4194304 | 6291456–6291456 | 4 |
| prototype paced | 기본 | target | 5254 → 53034 | 1 | 4194304 | 131072–131072 | 4 |
| prototype paced | 기본 | source | 53028 → 5254 | 1 | 4194304 | 131072–131072 | 0 |
| prototype paced | 기본 | source | 53034 → 5254 | 1 | 4194304 | 4318117–4318117 | 0 |
| prototype paced | 50k I/O 전용 | target | 5254 → 54144 | 1 | 4194304 | 6291456–6291456 | 4 |
| prototype paced | 50k I/O 전용 | target | 5254 → 54156 | 1 | 4194304 | 131072–131072 | 4 |
| prototype paced | 50k I/O 전용 | source | 54144 → 5254 | 1 | 4194304 | 131072–131072 | 0 |
| prototype paced | 50k I/O 전용 | source | 54156 → 5254 | 1 | 4194304 | 3086259–3086259 | 0 |
| binding paced | 기본 | target | 5247 → 39594 | 1 | 4194304 | 4611526–4611526 | 4 |
| binding paced | 기본 | target | 5247 → 39598 | 1 | 4194304 | 131072–131072 | 4 |
| binding paced | 기본 | source | 39594 → 5247 | 1 | 4194304 | 131072–131072 | 0 |
| binding paced | 기본 | source | 39598 → 5247 | 1 | 4194304 | 5669054–5669054 | 0 |
| binding paced | 50k I/O 전용 | target | 5247 → 40780 | 1 | 4194304 | 6291456–6291456 | 4 |
| binding paced | 50k I/O 전용 | target | 5247 → 40794 | 1 | 4194304 | 131072–131072 | 4 |
| binding paced | 50k I/O 전용 | source | 40780 → 5247 | 1 | 4194304 | 131072–131072 | 0 |
| binding paced | 50k I/O 전용 | source | 40794 → 5247 | 1 | 4194304 | 6291456–6291456 | 0 |

기본 61k 실행의 kernel TCP cycles/B는 thread별 10초 cycles:k hardware counter에 TCP/IP/socket 하위 stack의 cycle sample period 비율을 적용하고, 같은 실행의 완료 rate × 10초 × 해당 방향의 capture record B로 나눈 추정이다. Perf timestamp와 Python monotonic의 clock 원점이 달라 절대 timestamp 교집합으로 표본을 거르지 않았다. Kernel 밖의 memcpy는 이 값에 포함하지 않았다. 같은 thread의 cycles:k counter도 `capture/*.cycles.csv`에 보존했다. Sampling과 수집 경계의 in-flight 차이가 있으므로 소수점 이하까지 정확한 hardware 계수로 해석하지 않는다.

| 실행 | thread | kernel TCP cycles/B 추정 | kernel TCP cycles/request 추정 | TCP/kernel 표본 |
|---|---|---|---|---|
| main Framework | source IO/0 | 14.211 | 20,392.886 | 275/364 |
| main Framework | source IO/2 | 9.711 | 14,013.138 | 193/334 |
| main Framework | target IO/0 | 5.771 | 8,281.571 | 138/282 |
| main Framework | target IO/1 | 16.243 | 23,438.293 | 279/380 |
| prototype unpaced | source IO/0 | 17.146 | 18,723.729 | 241/321 |
| prototype unpaced | source IO/2 | 12.346 | 13,481.972 | 193/350 |
| prototype unpaced | target IO/0 | 8.006 | 8,742.010 | 113/246 |
| prototype unpaced | target IO/1 | 21.555 | 23,537.753 | 290/385 |
| prototype paced | source IO/0 | 17.781 | 19,416.325 | 217/305 |
| prototype paced | source IO/2 | 12.926 | 14,115.186 | 188/344 |
| prototype paced | target IO/0 | 10.092 | 11,020.787 | 117/248 |
| prototype paced | target IO/1 | 20.055 | 21,899.777 | 272/376 |
| binding paced | source IO/0 | 12.071 | 15,113.021 | 176/259 |
| binding paced | source IO/2 | 4.282 | 5,360.689 | 40/114 |
| binding paced | target IO/0 | 7.402 | 9,267.076 | 118/211 |
| binding paced | target IO/1 | 9.007 | 11,277.343 | 92/158 |

Receive allocator 계측은 로드한 libzlink.so의 `zmp_decoder_t::size_ready` 진입·반환 uprobe를 사용했다. 실제 라이브러리의 disassembly로 allocator 범위와 storage type 위치를 확인했다. Part 전체가 allocator 범위 안에 들어가는지와 성공 반환 후 storage type을 셌다. 아래는 probe가 활성화된 약 5초의 비율이며, schedstat snapshot 약 3초와 나눠 per-request 비용을 계산하지 않았다. Type 101은 inline storage, 105는 reference-counted storage다. 105라는 사실만으로 allocator 범위 안의 zero-copy 성공이라고 세지 않았다.

| 실행 | 수신 process | native part B | allocator 범위 안 | 범위 밖 | 범위 밖 비율 | storage type |
|---|---|---|---|---|---|---|
| main Framework | target | 13 | 322439 | 419 | 0.13% | 101 |
| main Framework | target | 1398 | 277386 | 45471 | 14.08% | 105 |
| main Framework | source | 13 | 2 | 0 | 0.00% | 101 |
| main Framework | source | 21 | 321903 | 818 | 0.25% | 101 |
| main Framework | source | 1398 | 268217 | 54505 | 16.89% | 105 |
| prototype paced | target | 13 | 2 | 0 | 0.00% | 101 |
| prototype paced | target | 41 | 316940 | 1191 | 0.37% | 105 |
| prototype paced | target | 1027 | 309747 | 8384 | 2.64% | 105 |
| prototype paced | source | 13 | 2 | 0 | 0.00% | 101 |
| prototype paced | source | 41 | 317615 | 1198 | 0.38% | 105 |
| prototype paced | source | 1027 | 307307 | 11506 | 3.61% | 105 |
| binding paced | target | 201 | 317403 | 90 | 0.03% | 105 |
| binding paced | target | 1027 | 316997 | 496 | 0.16% | 105 |
| binding paced | source | 201 | 317279 | 211 | 0.07% | 105 |
| binding paced | source | 1027 | 316476 | 1014 | 0.32% | 105 |

Mesh source의 JFR은 profile 설정으로 active 60초를 기록했다. 기존 jio와 같이 stack에서 먼저 만나는 registry take, reply header decode, multipart view decode, completion handoff, other에 한 번만 배정했다. 같은 실행의 mesh schedstat CPU에 표본 비율을 적용했다. Binding에는 Framework mesh thread가 없다. Prototype의 새 descriptor 해석은 other에 포함한다.

| mesh source JFR 분류 µs (표본) | main Framework | prototype unpaced | prototype paced |
|---|---|---|---|
| registry take | 1.211 (65/664) | 0.000 (0/704) | 0.000 (0/552) |
| reply decode | 1.025 (55/664) | 0.000 (0/704) | 0.000 (0/552) |
| multipart view decode | 0.578 (31/664) | 0.000 (0/704) | 0.000 (0/552) |
| completion handoff | 2.869 (154/664) | 3.278 (223/704) | 3.462 (173/552) |
| other | 6.689 (359/664) | 7.071 (481/704) | 7.585 (379/552) |

Main의 registry take·reply decode·multipart decode 합계는 2.813 µs지만 mesh CPU의 실제 감소는 1.324 µs다. Completion handoff와 other가 늘었으므로 제거한 단계의 합계를 순 절감으로 보고하지 않는다.

직접 변경한 worktree 파일은 다음과 같다. 기존 CPU-pin 패치와 raw Java pacing 옵션은 유지했다. BenchOptions.java와 다른 언어 runner의 기존 변경은 이번 작업에서 만들지 않았다.

| 파일 | 변경 이유 |
|---|---|
| framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/messaging/ZLinkChannelEnvelope.java | 기존 Header 타입에 compact descriptor encode/decode를 구현한다. |
| framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelCallRuntime.java | Node/mesh Channel의 descriptor 생성과 reply 해석을 명시적으로 연결한다. ClientServer 해석 경로는 기존 호출로 구분한다. |
| framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelRouteCalls.java | Node와 mesh Channel 요청이 native descriptor와 typed payload를 만들도록 연결한다. |
| framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java | Public binding request completion으로 결과를 받고 원래 reply token으로 native parts를 회신한다. 일반 hop의 registry 등록과 service correlation echo를 제거한다. |
| framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkMeshApplicationDispatcher.java | Native descriptor로 typed dispatch하고 성공 reply descriptor를 만든다. |
| framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistry.java | 기존 prepared Operation의 typed receiver와 observer를 binding terminal에서 직접 완료한다. Registry entry를 추가하지 않는다. |
| framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/BenchDrivers.java | 기존 request rate pacing을 Framework backpressure loop에도 적용한다. |

일반 요청 경로에서 service request/reply header, 75 B multipart wrapper, packed part 수와 길이 해석, JSON header encode/decode, hop matching용 `submitTerminal` 등록과 `take` 조회를 제거했다. 호출이 없어진 request overload 2개와 decodeRequestReply helper도 삭제했다. Typed payload serializer와 typed handler dispatch는 유지한다. 추가한 것은 descriptor codec, 명시적인 Node/Channel transport 경로 선택, 기존 Operation의 direct binding completion 진입점, bench pacing 연결이다. Durable request와 다른 경로의 registry 구현은 남겨 뒀다.

리팩토링 점검은 중복 service header, wrapper, JSON header, hop registry의 4항목을 확인하고 이 경로에서 제거·대체했다. 이 변경으로 호출이 없어진 helper 3개도 삭제했다. 범위 밖으로 이관한 항목은 0이다. 새 queue, lock, cache, persistent state는 0이다. 일반 hop의 요청·응답 매칭 판정 위치는 binding matching + service correlation 검사 + Framework registry 조회의 3곳에서 binding matching 1곳으로 줄었다. 기존 reply winner와 execution gate를 재사용했다.

Main은 소스 수정 전에 `/root/jobs/fx/cx/measure-shared.sh taskset -c 0-7 bash java/build.sh`로 빌드하고 runnable install output을 `main-build/`에 복사했다. 기준 commit은 사용자가 지정한 213e579f52다. Git metadata 조회는 runner의 stub으로 막았으므로 results.json의 commit은 unknown이다. Prototype도 같은 명령으로 빌드했다. 필요한 Framework Java composite build가 모두 같은 wrapper 안에서 수행됐다. 최종 descriptor 검사는 41 B와 payload 1,027 B, 128-bit identity 전체 보존을 확인했고 잘린 입력 41개, 이전 major 1개, 잘못된 operation 1개를 거부했다. 정식 cross-language/Spot/Actor/ClientServer 호환성, metadata/flow on, 오류·취소·deadline 일반 계약은 이 실험의 완료 조건에 포함하지 않았다.

소유 계층은 Core/binding의 transport matching, reply token, completion, multipart와 Framework의 논리 대상, typed dispatch, end-to-end identity, flow, metadata, deadline으로 나눴다.

기준 조항은 design.md 5절, binding async-execution-model 4·5절, wire protocol 2·4·11절, flow-correlation 2·3·5절이다. 새 descriptor는 사용자 승인 범위의 측정용 형식이며 정식 스펙은 수정하지 않았다.

교차언어 대조는 제공된 design.md의 C++/.NET/JVM/Node 현행 wire 대조를 기준으로 했다. 다른 언어 구현이나 parity test는 변경·실행하지 않았다.

변경 분류는 D에 해당하는 새 application wire 실험이다. 사용자에게 승인받은 Java 측정용 프로토타입이며 배포·commit 대상이 아니다.

재현 자료는 `runs/<조건>/results.json`, `wire.pcap`, `wire.json`, `capture/`, `capture-bpf/`, `capture-decoder/`, `source.jfr`에 있다. `main-build.log`, `prototype-build.log`, `descriptor-check.log`, `build-manifest.json`과 `prototype.diff`를 보존했다. 앞선 all-thread profiler 진단 run과 중단된 수집기·smoke 실행은 최종 표에 합산하지 않았다. 기본 성능 조건 4개는 각 1회다. 61k I/O 재계수 3개와 50k 원인 확인 3개도 각각 1회이며 모두 요청 누락·오류·abandoned가 0이다. 남은 실행 실패는 없다. 종료에는 SIGTERM/SIGINT를 사용했고 shell background `&` 실행은 사용하지 않았다.
