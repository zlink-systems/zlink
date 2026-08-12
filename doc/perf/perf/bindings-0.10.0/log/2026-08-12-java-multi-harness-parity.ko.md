# Java multi harness C parity 정렬 결과

## 변경 범위

Java multi runner에서 C와 달랐던 latency reservoir, DEALER/DEALER deadline,
routed relay 종료, PUB/SUB stop token, STREAM backpressure·timeout을 C 기준으로
정렬했다. 공개 binding interface는 변경하지 않았다.

STREAM은 private pending queue로 `DONT_WAIT` send의 transient backpressure를
보존하고 send-ready callback에서 같은 routing id와 native `Message`를 순서대로 재전송한다.
queue는 packet callback과 send-ready callback 사이에서 lock으로 보호한다.

## 개별 재측정 결과

Release Core `0.10.1`, `tcp`, clients `100`, duration `1초`, runs `1`, balanced
auto-HWM에서 C를 먼저, Java를 다음에 단독 실행했다.

| Pattern | C 대비 throughput 비율 (size 순서) | 산술평균 | 판정 기준 |
|---|---|---:|---|
| `MULTI_DEALER_ROUTER_SENDSEND` | 51.19 / 51.17 / 52.36 / 55.15 / 117.45 / 104.06% | 71.90% | multi routed echo 목표 70% 통과 |
| `MULTI_DEALER_DEALER` | 52.85 / 68.37 / 93.33 / 66.47 / 51.17 / 75.65% | 67.97% | 단순 one-way 목표 90% 미달 |
| `MULTI_PUBSUB` | 39.08 / 44.13 / 48.73 / 49.57 / 58.33 / 71.98% | 52.64% | 단순 one-way 목표 90% 미달 |
| `MULTI_STREAM` | 73.87 / 72.57 / 66.41 / 92.46% | 76.33% | STREAM 재측정값 |

## 결과 파일

- DEALER/ROUTER C: `/tmp/zlink-java-relay-parity-c/multi/report/perf_c_multi_linux_20260812_222232.txt`
- DEALER/ROUTER Java: `/tmp/zlink-java-relay-parity-java/multi/report/perf_java_multi_linux_20260812_222251.txt`
- DEALER/DEALER C: `/tmp/zlink-java-dd-parity-c/multi/report/perf_c_multi_linux_20260812_222507.txt`
- DEALER/DEALER Java: `/tmp/zlink-java-dd-parity-java/multi/report/perf_java_multi_linux_20260812_222529.txt`
- PUB/SUB C: `/tmp/zlink-java-pub-parity-c/multi/report/perf_c_multi_linux_20260812_222635.txt`
- PUB/SUB Java: `/tmp/zlink-java-pub-parity-java/multi/report/perf_java_multi_linux_20260812_222655.txt`
- STREAM C: `/tmp/zlink-java-stream-parity-c/multi/report/perf_c_multi_linux_20260812_222433.txt`
- STREAM Java: `/tmp/zlink-java-stream-parity-java/multi/report/perf_java_multi_linux_20260812_222451.txt`

## 후속 종료 경계 정렬

기존 DEALER/DEALER server는 active drain 중 stop token을 소비한 뒤 빈
`poll(-1)`에 다시 진입할 수 있었다. 이 경우 configured active duration이 끝나도
process가 종료되지 않아 결과가 partial이 된다. C와 Java는 stop token을 본 뒤
active deadline의 남은 시간만 대기하도록 같은 상태 전이를 사용한다.

routed echo는 C comparison runner가 client 완료 뒤 server stdin에 `STOP`을
전달한다. Java runner에도 같은 전달을 추가했다. Java client가 C에 없는 routed
stop frame을 추가 전송하던 경로는 제거했다. relay receive drain은 C와 같이 각
iteration에서 runner 종료 상태를 확인한다.

payload padding 전체를 Java에서 채우던 작업도 C처럼 metric header만 기록하도록
정렬했다. 이 변경 전 partial report는 비교에 사용하지 않는다.

Release Core `0.10.1`, `tcp`, clients `100`, duration `5초`, runs `1`, balanced
auto-HWM의 DEALER/DEALER paired run은 complete다. size 순서 `64/256/1024/4096/
65536/131072B`의 C 대비 throughput 비율은
`67.56 / 83.33 / 81.92 / 74.75 / 84.15 / 96.18%`, 산술평균은 `81.32%`다.

- C: `/tmp/zlink-java-dd-final-c/multi/report/perf_c_multi_linux_20260812_234734_java-dd-final-c.txt`
- Java: `/tmp/zlink-java-dd-final-java/multi/report/perf_java_multi_linux_20260812_234813_java-dd-final-java.txt`

같은 조건의 DEALER/ROUTER SENDSEND paired run도 complete다. size 순서
`64/256/1024/4096/65536/131072B`의 C 대비 throughput 비율은
`76.71 / 60.33 / 61.58 / 60.66 / 105.08 / 96.73%`, 산술평균은 `76.85%`다.

- C: `/tmp/zlink-java-dr-final-c/multi/report/perf_c_multi_linux_20260812_235616_java-dr-final-c.txt`
- Java: `/tmp/zlink-java-dr-final-java/multi/report/perf_java_multi_linux_20260812_235651_java-dr-final-java.txt`

DEALER/ROUTER REQREP paired run도 complete다. size 순서
`64/256/1024/4096/65536/131072B`의 C 대비 throughput 비율은
`68.20 / 62.14 / 49.81 / 53.55 / 101.99 / 107.66%`, 산술평균은 `73.89%`다.

- C: `/tmp/zlink-java-drreq-final-c/multi/report/perf_c_multi_linux_20260812_235810_java-drreq-final-c.txt`
- Java: `/tmp/zlink-java-drreq-final-java/multi/report/perf_java_multi_linux_20260812_235849_java-drreq-final-java.txt`

PUB/SUB paired run도 complete다. size 순서 `64/256/1024/4096/65536/131072B`의
C 대비 throughput 비율은 `66.42 / 46.57 / 51.48 / 58.25 / 68.91 / 71.33%`,
산술평균은 `60.49%`다.

- C: `/tmp/zlink-java-pub-final-c/multi/report/perf_c_multi_linux_20260813_000013_java-pub-final-c.txt`
- Java: `/tmp/zlink-java-pub-final-java/multi/report/perf_java_multi_linux_20260813_000050_java-pub-final-java.txt`

STREAM paired run도 complete다. size 순서 `64/256/1024/65536B`의 C 대비
throughput 비율은 `88.82 / 74.36 / 77.56 / 114.70%`, 산술평균은 `88.86%`다.

- C: `/tmp/zlink-java-stream-final-c/multi/report/perf_c_multi_linux_20260813_000209_java-stream-final-c.txt`
- Java: `/tmp/zlink-java-stream-final-java/multi/report/perf_java_multi_linux_20260813_000237_java-stream-final-java.txt`

## 미채택 receive 경계 호출 축소 결과

`zlink_msg_size`와 `zlink_msg_data`를 하나의 Java private bridge 호출로 묶는
PUB/SUB 수신 materialization 변경을 같은 조건의 64B·256B로 비교했다. C와 Java를
각 size에서 한 번씩 순서대로 실행했다. C 대비 throughput은 각각 `66.30%`,
`66.20%`였으며, 기존 구현의 대응 측정값보다 낮았다. 이 변경은 성능 개선으로
채택하지 않고 제거했다.

- C: `/tmp/zlink-java-pub-recvmeta-c/multi/report/perf_c_multi_linux_20260813_002209_java-pub-recvmeta-c.txt`
- Java: `/tmp/zlink-java-pub-recvmeta-java/multi/report/perf_java_multi_linux_20260813_002228_java-pub-recvmeta-java.txt`

## 미채택 sender header 생성 후보 결과

Java sender의 metric header sequence를 전역 `AtomicLong` 대신 producer-local
counter로 바꾸는 후보를 PUB/SUB 전체 size에서 비교했다. C 대비 throughput 비율은
`64.20 / 52.05 / 49.82 / 59.36 / 58.56 / 65.10%`, 산술평균은 `58.18%`였다.
기존 공식 `60.49%`보다 낮으므로 이 후보는 제거했다.

- C: `/tmp/zlink-java-seqlocal-clean-pub-c/multi/report/perf_c_multi_linux_20260813_003956_java-seqlocal-clean-pub-c.txt`
- Java: `/tmp/zlink-java-seqlocal-clean-pub-java/multi/report/perf_java_multi_linux_20260813_004032_java-seqlocal-clean-pub-java.txt`

새 Message 직후의 고정 header write를 생략하는 후보도 64B·256B에서 각각
`66.02%`, `55.09%`로, 원래 구현의 대응 측정값 `71.34%`, `71.29%`보다 낮았다.
이 후보도 제거했다.

- 후보 C: `/tmp/zlink-java-headeronce-pub-c/multi/report/perf_c_multi_linux_20260813_003424_java-headeronce-pub-c.txt`
- 후보 Java: `/tmp/zlink-java-headeronce-pub-java/multi/report/perf_java_multi_linux_20260813_003500_java-headeronce-pub-java.txt`
- 원래 구현 C: `/tmp/zlink-java-headerbaseline-pub-c/multi/report/perf_c_multi_linux_20260813_003621_java-headerbaseline-pub-c.txt`
- 원래 구현 Java: `/tmp/zlink-java-headerbaseline-pub-java/multi/report/perf_java_multi_linux_20260813_003636_java-headerbaseline-pub-java.txt`

## 채택한 PUB/SUB header 단일 decode 결과

Java PUB/SUB receiver는 active frame마다 phase 검사와 latency 기록에서 같은 header를
두 번 읽었다. C처럼 한 번 검사해 phase와 timestamp를 함께 처리하도록 바꿨다. 전체
size의 C 대비 throughput 비율은 `51.12 / 49.28 / 62.27 / 80.50 / 79.33 / 92.31%`,
산술평균은 `69.14%`다. 이전 공식 평균 `60.49%`보다 높아 이 변경을 채택했다.

- C: `/tmp/zlink-java-pub-singledecode-full-c/multi/report/perf_c_multi_linux_20260813_004624_java-pub-singledecode-full-c.txt`
- Java: `/tmp/zlink-java-pub-singledecode-full-java/multi/report/perf_java_multi_linux_20260813_004700_java-pub-singledecode-full-java.txt`

## 미채택 TopicMessage single-part storage 재사용 결과

caller-provided `TopicMessage`가 보관한 single-part Message를 다음 수신에서 다시
사용하는 후보를 PUB/SUB 전체 size에서 비교했다. C 대비 throughput 비율은
`77.06 / 57.07 / 56.72 / 64.69 / 77.29 / 71.11%`, 산술평균은 `67.32%`였다.
직전 공식 평균 `69.14%`보다 낮으므로 이 후보는 제거했다.

- C: `/tmp/zlink-java-topicreuse-full-c/multi/report/perf_c_multi_linux_20260813_005213_java-topicreuse-full-c.txt`
- Java: `/tmp/zlink-java-topicreuse-full-java/multi/report/perf_java_multi_linux_20260813_005249_java-topicreuse-full-java.txt`

## 채택한 TopicMessage two-slot 재사용 결과

single-part 수신에서 현재 공개 part와 다음 native receive candidate를 분리해
교대로 사용하도록 바꿨다. 성공 전 candidate는 `parts()`에 나타나지 않으며,
EAGAIN/EINTR에서는 기존 결과가 유지된다. 전체 size의 C 대비 throughput 비율은
`78.11 / 58.42 / 60.59 / 72.85 / 79.43 / 78.60%`, 산술평균은 `71.33%`다.
직전 공식 평균 `69.14%`보다 높아 이 변경을 채택했다.

- C: `/tmp/zlink-java-topic-two-slot-full-c/multi/report/perf_c_multi_linux_20260813_005827_java-topic-two-slot-full-c.txt`
- Java: `/tmp/zlink-java-topic-two-slot-full-java/multi/report/perf_java_multi_linux_20260813_005903_java-topic-two-slot-full-java.txt`

## 채택한 PUB direct send 결과

PUB server가 active payload의 shared copy를 만들어 보내던 경로를, 원래 payload frame을
직접 전송하고 다음 iteration에서 새 frame을 준비하도록 바꿨다. send 성공 뒤 Message를
재사용하지 않는 public ownership 계약과 같다. 전체 size의 C 대비 throughput 비율은
`75.74 / 63.29 / 54.24 / 70.83 / 80.47 / 84.05%`, 산술평균은 `71.44%`다.
two-slot 수신만 적용한 평균 `71.33%`보다 높아 이 변경을 채택했다.

- C: `/tmp/zlink-java-pub-direct-two-slot-full-c/multi/report/perf_c_multi_linux_20260813_010127_java-pub-direct-two-slot-full-c.txt`
- Java: `/tmp/zlink-java-pub-direct-two-slot-full-java/multi/report/perf_java_multi_linux_20260813_010205_java-pub-direct-two-slot-full-java.txt`

## Java harness 정책 재검증

원래 코드로 복원한 뒤 tcp `MULTI_PUBSUB` 131072B 한 케이스를 C 다음 Java 순서로
다시 실행했다. 두 runner 모두 100 clients, 5초 active duration, auto-HWM, I/O thread
4, 기본 socket buffer와 release Core 0.10.1을 사용했고 complete report를 만들었다.
C throughput은 `53,956.4 msg/s`, Java throughput은 `62,448.8 msg/s`로 C 대비
`115.74%`였다. 이 단일 케이스는 변경 채택 판정이나 전체 평균 갱신에 사용하지 않는다.

- C: `/tmp/zlink-java-pub-131072-c/multi/report/perf_c_multi_linux_20260813_011539_java-pub-131072-c.txt`
- Java: `/tmp/zlink-java-pub-131072-java/multi/report/perf_java_multi_linux_20260813_011550_java-pub-131072-java.txt`

## 미채택 Received two-slot 수신 wrapper

Java routed caller-provided `Received`에도 public single-part와 다음 native receive
candidate를 교대하는 후보를 적용했다. Router 관련 테스트와 perf build는 통과했지만,
동일 조건의 DEALER/DEALER 비교에서 131072B client가 `zlinksubmitexception`으로 실패해
complete report를 만들지 못했다. 앞 size throughput도 기존 공식 결과보다 낮았으므로 이
후보는 즉시 제거했다. 결과 표와 평균 판정에는 사용하지 않는다.

- C: `/tmp/zlink-java-dd-twoslot-c/multi/report/perf_c_multi_linux_20260813_013845_java-dd-twoslot-c.txt`
- Java 임시 로그: `/tmp/zlink-java-dd-twoslot-java/multi/tmp/`
