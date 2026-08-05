# 연결당 메모리 사용량 분석과 절감 제안

이 문서는 zlink core가 서버 간 연결을 수천~수만 개 유지할 때 연결 하나가
기본으로 차지하는 메모리를 분석하고, 성능을 유지하면서 이를 줄일 수 있는
방안을 제안한다. 수치는 코드 정적 분석과 `core/study`의 실측 프로그램으로
직접 측정한 결과를 함께 싣는다.

- 측정일: 2026-07-10
- 대상: libzlink 8.6.3 (Release 빌드, boost::asio proactor 엔진, TLS 활성)
- 환경: Linux 6.6 (WSL2), x86-64, glibc, 20 core / 94 GB
- 측정 도구: 이 디렉토리의 study 프로젝트 (재현 방법은 §7)
- 표기 규약: 문서의 kB는 `/proc` 기준 KiB이고, MB/GB는 그 값을 10³씩
  나눈 표기다 (십진 단위 대비 약 2.4 % 작게 적힌다). 비율·기울기·절감률은
  이 표기 방식과 무관하게 성립한다.

## 요약

| 항목 | 결과 |
|------|------|
| idle 연결 1개가 실제 점유하는 RSS | **약 34~40 KB** (소켓 패턴별 차이 작음) |
| idle 연결 1개에 할당되는 힙(가상 기준) | **약 75 KB** (아직 안 쓴 페이지 포함) |
| 트래픽을 겪은 뒤 연결당 RSS | **약 42 KB+** (버퍼 페이지가 커밋되며 늘고, 다시 줄지 않음) |
| SpotNode mesh peer 1개 (ALL 모드) | **약 106 KB + TCP 3연결** |
| SpotNode mesh peer 1개 (PUBSUB 모드) | **약 64 KB + TCP 2연결** |
| 커널 쪽 (소켓 버퍼) | idle에서는 거의 0 (버퍼는 in-flight 데이터에만 커밋) |
| 가장 큰 소비처 | pipe 큐 청크 2×16.4 KB, 코덱 버퍼 24.4 KB, 핸드셰이크 버퍼 8 KB |
| 실험으로 검증한 절감 | granularity=16 실험만으로 idle RSS −17 % (§6.2) |
| **구현 적용 실측 (P1+P2)** | idle 연결당 **35.6 → 28.0 KB (−21 %)**, 10,000연결 idle 365 → 289 MB. tcp/1024 perf 회귀 미검출(노이즈 범위 내, §6.8) |

10,000연결 직접 실측(§4.1~4.2) 기준으로, 서버 프로세스는 baseline에서
idle 354~418 MB(패턴별), ROUTER 기준 트래픽 후 432 MB를 쓴다. P1+P2
적용 후에는 idle 274~318 MB, ROUTER 트래픽 후 340 MB다. 5만 연결로
외삽하면 baseline idle 1.8 GB → P1+P2 1.4 GB다 (§4.4). 나머지
제안(P3~P4)까지 적용하면 idle 기준 연결당 20 KB 안팎까지 낮출 수 있다고
추정한다.

## 1. 연결 하나가 만들어질 때 무엇이 할당되는가

TCP 연결 하나가 accept(또는 connect)되면 다음 순서로 객체가 생긴다.

```text
listener accept
  └─ asio_zmp_engine_t 생성 (raw STREAM이면 asio_raw_engine_t)
       └─ session_base_t 생성
            └─ engine plug → 핸드셰이크
                 └─ pipepair(): pipe_t 2개 + ypipe 2개 (소켓 ↔ 세션 큐)
                 └─ 핸드셰이크 완료 시 encoder/decoder 버퍼 할당
```

### 1.1 항목별 메모리 (idle ZMP 연결 1개, 기본 옵션)

"할당"은 malloc 기준 크기, "실제 점유"는 페이지가 커밋되어 RSS에 잡히는
근사값이다. 할당했더라도 아직 쓰지 않은 페이지는 RSS에 잡히지 않기 때문에
두 값이 다르다.

| 항목 | 할당 시점 | 크기 공식 | 할당 (B) | 실제 점유 (B, 근사) |
|------|----------|----------|---------|----------------------|
| ypipe 큐 청크 ×2 | pipepair 생성자 (즉시) | `message_pipe_granularity(256) × sizeof(msg_t)(64) + 16` ×2 | 32,800 | ~8,000 (첫 페이지만 touch) |
| ZMP decoder 버퍼 | 핸드셰이크 완료 시 | `in_batch_size(8192) + 8 + ceil(8192/41)×40` | 16,200 | ~4,000 (수신량만큼 커밋) |
| ZMP encoder 버퍼 | 핸드셰이크 완료 시 | `out_batch_size(8192)` | 8,192 | 0~4,000 (송신해야 커밋) |
| 엔진 read_buffer | 엔진 생성자 (즉시) | `read_buffer_size(8192)`, `vector::resize`가 0으로 채움 | 8,192 | **8,192 (zero-fill로 전부 커밋)** |
| asio_zmp_engine_t 객체 | accept 시 | handler_allocator 3×1,040 + options_t 사본(936 B) + HELLO 버퍼 272×2 + rid 256 등 | 5,856 (sizeof 실측) | ~5,900 |
| pipe_t 객체 ×2 | pipepair | 내장 stream packet 상태 ~184 B, msg_t 2개 포함 | 1,280 (sizeof 실측 640×2) | ~1,300 |
| session_base_t | accept 시 | options_t 사본(936 B) 포함 | 1,216 (sizeof 실측) | ~1,200 |
| ypipe_t 객체 ×2 | pipepair | | ~160 | ~160 |
| tcp transport + boost socket | accept 시 | shared_ptr 제어블록 포함 | ~200 | ~200 |
| metadata_t + map 노드 | 핸드셰이크 | 항목 3~4개 | ~200 | ~200 |
| ROUTER `_out_pipes` 항목 | pipe attach | rbtree 노드 2개 + routing id blob | ~150~400 | ~150~400 |
| epoll descriptor_state (boost) | 첫 async 연산 | | ~300 | ~300 |
| **합계** | | | **≈ 75 KB** | **≈ 30 KB + malloc 오버헤드** |

객체 크기는 이 빌드(TLS 활성)의 `sizeof` 실측값이다. 합계는 실측
idle 연결당 RSS 34~40 KB(§4)와 잘 맞는다. 나머지 차이는 malloc 청크
헤더와 arena 단편화, 그리고 아직 touch되지 않은 페이지다.

핵심 관찰 세 가지:

1. **pipe 큐 청크가 가장 크다.** 연결마다 256칸(각 64 B) 청크 2개를
   생성 시점에 즉시 할당한다. 대규모 연결에서는 auto-HWM이 pipe 예산을
   8~32 메시지로 줄이는데(§5), 청크는 여전히 256칸이라 실제로 쓸 수 있는
   깊이보다 8~32배 크다.
2. **엔진 read_buffer 8 KB는 핸드셰이크 전용이다.** decoder가 만들어진
   뒤에는 새 수신이 모두 decoder 버퍼로 직접 들어간다 (`asio_engine.cpp`의
   버퍼 선택 로직: decoder가 없을 때만 `_pipeline.read_buffer`에 읽는다).
   단 핸드셰이크 꼬리에 붙어 온 잔여 바이트는 소진될 때까지 이 버퍼 안에
   남아 있을 수 있다(§6.1의 해제 시점 조건). 그리고 `resize()`가 0으로
   채우기 때문에 8 KB 전체가 처음부터 RSS에 커밋된다.
3. **코덱 버퍼는 트래픽이 지나가며 점점 커밋된다.** idle+메시지 1개
   상태에서는 일부만 커밋되지만, 한 번 touch된 페이지는 반환되지 않는다
   (§4.2 실측). 따라서 "부하 후 안정 상태"의 연결당 비용은 할당
   합계(≈75 KB)를 상한으로 보고 계획하는 것이 안전하다. 이 상한은 정적
   분석에서 나온 추정값이다 — 전부 커밋되려면 양방향 트래픽과 깊은
   큐잉이 모두 있어야 하며, 그 상태를 직접 실측하지는 않았다.

### 1.2 raw STREAM 연결의 차이

STREAM 소켓은 생성자에서 배치 크기를 스스로 줄인다
(`in_batch_size = 4,160`, `out_batch_size = 4,096` — `stream.cpp`,
`stream_batch_policy.hpp`). 그래서 코덱 버퍼가 ZMP의 절반 이하다.

| 항목 | ZMP | raw STREAM |
|------|-----|------------|
| decoder | 16,200 B, 핸드셰이크 시 | 4,208 B (`4160+8+40`), plug 시 — 엔진이 수신을 즉시 걸며 버퍼를 확보하므로 지연되는 것은 페이지 커밋뿐이다. rcvbuf/maxmsgsize까지 성장 |
| encoder | 8,192 B | 4,096 B |
| HELLO/rid 버퍼 | 272+272+256 B | 없음 |
| 실측 idle RSS | 35.6 KB/conn | 34.3 KB/conn |

## 2. SpotNode mesh peer의 비용

SpotNode는 소켓 묶음(mesh-pub, mesh-xsub, external-router, peer_ctrl_pub,
peer_ctrl_sub, fanout, ctrl, pub_ingress_sub)을 노드당 한 번만 만든다.
peer가 늘어날 때 소켓이 늘어나는 것이 아니라, **공유 소켓 위에 TCP 연결과
pipe가 늘어난다.**

`connect_peer` 1회(A→B)당 생기는 TCP 연결:

| A쪽 소켓 | B쪽 소켓 | 모드 |
|----------|----------|------|
| mesh_xsub (XSUB) | mesh_pub (PUB) | PUBSUB/ALL |
| routed_router (ROUTER) | routed_router (ROUTER) | ROUTED/ALL |
| peer_ctrl_pub (PUB) | peer_ctrl_sub (SUB) | 항상 |

즉 ALL 모드는 peer당 TCP 3연결, PUBSUB 모드는 2연결이다. 양방향 mesh면
그 두 배가 된다. 각 TCP 연결은 §1.1의 연결 비용을 그대로 지불하고, 여기에
peer 테이블(map/set 항목 10~15개, 항목당 rbtree 노드 ~48 B + endpoint
문자열)이 1~2 KB 더해진다.

실측 (hub 노드 기준, peer 128개까지):

| 모드 | peer당 RSS | peer당 fd | 구성 |
|------|-----------|-----------|------|
| ALL | **105.7 KB** | 3 | TCP 3연결 + peer 테이블 |
| PUBSUB | **64.3 KB** | 2 | TCP 2연결 + peer 테이블 |

SpotNode 자체의 고정 비용(연결 0개일 때)은 소켓 11~12개 + data plane
스레드 + dispatch worker 풀 + eventfd 15개 안팎으로, hub 프로세스 기준
baseline RSS ~10 MB에 포함되어 있었다 (노드 1개 증분은 수백 KB 수준).

## 3. auto-HWM과 메모리의 관계

auto-HWM은 **선할당이 아니라 상한(cap)**이다. idle 메모리에는 영향이
없고, 부하 시 pipe에 쌓일 수 있는 메시지 수만 제한한다.

- profile 기준값: COMPACT 64 / LOW_LATENCY 128 / BALANCED 256 / THROUGHPUT 512
- 연결 수 bucket이 peer가 늘수록 pipe당 예산을 줄인다
  (BALANCED 기준: ≤64 peer → 256, ≤128 → 128, ≤512 → 64, ≤2048 → 32, 그 이상 → 16)
- bucket 전환에는 hysteresis가 있다 (64-bucket에서 80 이상이면 다음
  bucket으로, 48 이하로 내려오면 복귀)

따라서 **수만 연결에서 부하 시 총 메모리 상한**은 auto-HWM이 이미 억제해
준다. 예: 5만 peer, BALANCED, 4 KB 단위면 pipe당 16×4 KB = 64 KB 상한.
문제는 idle 기본 비용(§1)이 HWM과 무관하게 고정으로 나간다는 점이고,
그중 pipe 청크는 HWM 대비 과대(§6.2)하다.

또한 이 상한에는 정확성 한계가 두 가지 있다. 집행이 메시지 개수
기준이라 msg_unit(4 KB)보다 큰 메시지에서는 바이트 노출이 예산을
넘을 수 있고, pipe당 예산의 합은 여전히 peer 수에 비례해 전역 상한이
없다. 보강 방안은 §6.7에 정리했다.

## 4. 실측 결과

서버 프로세스 하나에 클라이언트 프로세스(별도 프로세스)가 연결을
누적시키면서, 각 단계에서 서버의 `malloc_trim` 후 VmRSS를 기록했다.
모든 연결은 핸드셰이크 후 메시지 1개를 보낸 상태(코덱 버퍼가 살아 있는
현실적 idle)다. 클라이언트→서버 단방향이므로 서버 쪽 encoder 페이지는
touch되지 않은 상태라는 비대칭이 있다. 수치는 구성당 1회 실행이고
연결당 기울기는 구간 양 끝점 기준이다 — 한계는 §7 주의를 참고한다.

### 4.1 연결 수별 서버 RSS — 10,000연결 기준, baseline vs P1+P2 (§6.8)

값은 각 단계의 서버 총 RSS(MB)이고, 연결당 기울기는 첫 단계와 마지막
단계 사이의 증분이다. baseline은 P1·P2 적용 전 빌드, P1+P2는 적용 후
빌드다 (같은 하네스, 격리 빌드 A/B).

| 시나리오 | 빌드 | 0 | 1,000 | 2,000 | 4,000 | 6,000 | 10,000 | 연결당 |
|---|---|---|---|---|---|---|---|---|
| ROUTER ← DEALER | baseline | 9.6 | 44.3 | 79.5 | 151.0 | 222.2 | 365.1 | **35.6 KB** |
| ROUTER ← DEALER | P1+P2 | 9.8 | 37.3 | 65.4 | 120.8 | 177.3 | 289.3 | **28.0 KB** |
| STREAM ← raw TCP | baseline | 9.4 | 43.8 | 78.2 | 147.4 | 216.3 | 353.8 | **34.4 KB** |
| STREAM ← raw TCP | P1+P2 | 9.8 | 35.7 | 61.9 | 114.9 | 167.4 | 273.8 | **26.5 KB** |
| PUB ← SUB (구독 1개씩) | baseline | 9.6 | 47.5 | 87.8 | 169.2 | 253.6 | 418.1 | **41.2 KB** |
| PUB ← SUB (구독 1개씩) | P1+P2 | 9.8 | 37.0 | 69.5 | 130.1 | 194.2 | 317.5 | **31.2 KB** |

| SpotNode hub | 빌드 | 0 peer | 16 | 32 | 64 | 128 | peer당 |
|---|---|---|---|---|---|---|---|
| ALL 모드 | baseline | 9.9 | 11.0 | 12.6 | 15.7 | 22.9 | **105.7 KB** |
| ALL 모드 | P1+P2 | 10.2 | 11.4 | 12.3 | 14.6 | 20.2 | **78.6 KB** |
| PUBSUB 모드 | baseline | 10.1 | 10.7 | 11.7 | 13.8 | 17.9 | **64.3 KB** |
| PUBSUB 모드 | P1+P2 | 10.1 | 10.9 | 11.4 | 12.6 | 16.0 | **45.7 KB** |

### 4.2 기본 점유 vs 트래픽 후 잔류 vs 순간 최대 (10,000연결)

버스트 실험: ROUTER·STREAM은 연결마다 1 KB 메시지 20개를 클라이언트가
일제히 송신, PUB는 서버가 1 KB 메시지 20개를 10,000 구독자 전체에
발행(총 20만 건 팬아웃), 구독자는 계속 비운다(drain). 세 값의 의미:

- **기본 점유(idle)** — 연결 성립+메시지 1개 상태의 고정 비용
- **트래픽 후 잔류** — 버스트가 끝나고 큐가 빈 뒤에도 유지되는 RSS.
  한 번 touch된 버퍼 페이지는 되돌아오지 않으므로 **용량 계획의 기준선**
- **순간 최대(VmHWM)** — 버스트 중 큐잉 피크. auto-HWM이 상한을 정하며
  워크로드(메시지 크기×HWM slot) 의존. **메모리 리밋/OOM 설정 기준**

| 패턴 | 빌드 | 기본 점유 (연결당 / 총) | 트래픽 후 잔류 | 순간 최대 |
|---|---|---|---|---|
| ROUTER | baseline | 35.6 KB (365.1 MB) | 42.4 KB (432.4 MB) | 75.5 KB (763.8 MB) |
| ROUTER | P1+P2 | 28.0 KB (289.3 MB) | 33.1 KB (340.2 MB) | 65.6 KB (665.4 MB) |
| STREAM | baseline | 34.4 KB (353.8 MB) | 34.5 KB (354.7 MB) | 34.5 KB (354.7 MB) |
| STREAM | P1+P2 | 26.5 KB (273.8 MB) | 26.5 KB (274.6 MB) | 26.5 KB (274.6 MB) |
| PUB | baseline | 41.2 KB (418.1 MB) | 51.2 KB (518.3 MB) | 51.5 KB (521.0 MB) |
| PUB | P1+P2 | 31.2 KB (317.5 MB) | 36.0 KB (365.7 MB) | 36.3 KB (369.2 MB) |

읽을 점:

- ROUTER는 수신 큐·디코더가 트래픽으로 커밋되며 +6~7 KB/conn이 영구
  잔류하고, 버스트 중 피크는 idle의 2배를 넘는다. P1+P2가 잔류(−22 %)와
  피크(−13 %) 모두 줄인다.
- STREAM은 버스트 잔류가 거의 없다 — raw 경로는 코덱 버퍼가 작고(4 KB
  단위) 수신 즉시 콜백으로 비워지기 때문이다. 이 패턴은 idle 비용이
  사실상 전부다.
- PUB는 팬아웃 송신이라 잔류 증가가 가장 크다(baseline +10 KB/conn).
  P1+P2에서 잔류 증가가 절반 이하로 준다(+4.8 KB/conn) — 송신 pipe
  청크 축소 효과가 그대로 나타나는 지점이다.
- 이 버스트는 1회성 부하다. 지속적인 양방향 트래픽·깊은 큐잉을 겪는
  연결이 §1.1의 할당 합계(≈75 KB)까지 커밋된다는 것은 관측의 방향을
  연장한 상한 추정이지 실측이 아니다. 용량 계획은 상한 기준이 안전하다.

### 4.3 커널 쪽 소켓 버퍼

SO_SNDBUF/SO_RCVBUF 기본값은 `-1`(설정 안 함)이라 커널 autotuning을
따른다 (tcp_rmem 기본 87 KB, 최대 16 MB). 실측에서 **idle 연결의 TCP
버퍼 페이지는 거의 0**이었다 — 16,000 소켓(서버+클라이언트 합)에서
`/proc/net/sockstat` TCP mem이 1,365페이지(≈5.5 MB)에 그쳤고 버스트가
끝나면 다시 내려갔다. 커널 버퍼는 in-flight 데이터에만 커밋되므로
수만 연결의 idle 비용은 소켓 구조체 slab(연결당 수 KB 수준 — 일반적인
커널 지식이며 이번에 실측한 값은 아니다)이 전부다. 지속 부하에서는
tcp_mem/tcp_rmem/tcp_wmem sysctl이 상한을 결정한다. 이 관측은
loopback(WSL2) 기준이라 실제 NIC 경로에서는 절대값이 다를 수 있다.

### 4.4 규모 외삽 (ROUTER 서버 프로세스 기준)

10,000연결은 §4.1~4.2의 직접 실측값이고, 30,000/50,000은 그 기울기의
선형 외삽이다.

| 연결 수 | | idle | 트래픽 후 잔류 (실측) | 지속 부하 상한 (할당 기반 추정) |
|---------|---|------|--------------------------|------------------------------|
| 10,000 (실측) | baseline | 365 MB | 432 MB | ~0.75 GB |
| | P1+P2 | **289 MB** | **340 MB** | ~0.42 GB |
| 30,000 (외삽) | baseline | ~1.07 GB | ~1.27 GB | ~2.2 GB |
| | P1+P2 | ~0.84 GB | ~0.99 GB | ~1.3 GB |
| 50,000 (외삽) | baseline | ~1.78 GB | ~2.1 GB | ~3.7 GB |
| | P1+P2 | **~1.40 GB** | **~1.65 GB** | **~2.1 GB** |

SpotNode mesh는 peer당 TCP 2~3연결이므로 같은 peer 수라면 2~3배로 읽어야
한다 (예: ALL 모드 peer 10,000개 ≈ baseline 1.06 GB / P1+P2 0.79 GB +
fd 30,000개 — 128 peer까지의 실측 기울기를 선형 외삽한 값이다).

## 5. 어디를 줄일 수 있는가 — 구조적 여지

idle 연결당 ~35 KB(부하 시 상한 ~75 KB) 중 조정 가능한 몫:

```text
 할당 기준 ≈75 KB 의 구성
 ┌──────────────────────────────────────────────┐
 │ pipe 청크 2개        32.8 KB  (43.8 %)  ← §6.2 │
 │ decoder 버퍼         16.2 KB  (21.6 %)  ← §6.3 │
 │ encoder 버퍼          8.2 KB  (10.9 %)  ← §6.3 │
 │ 핸드셰이크 read_buffer 8.2 KB (10.9 %)  ← §6.1 │
 │ 엔진/세션/pipe 객체 등 9.5 KB (12.7 %)  ← §6.4 │
 └──────────────────────────────────────────────┘
```

## 6. 절감 제안

우선순위는 (절감폭 × 위험도 역수) 순이다. 모든 항목은 "성능 유지"를
전제로 하며, 커밋 전에 공식 perf 게이트(baseline vs patched 비교)가
필요하다.

### 6.1 [P1] 핸드셰이크 read_buffer 축소/해제 — 연결당 −8 KB

- **현상**: `asio_engine_t` 생성자가 `_pipeline.read_buffer`를 8,192 B로
  `resize()`한다. zero-fill 때문에 전부 RSS에 커밋된다. 새 read를 걸 때의
  버퍼 선택은 decoder가 있으면 항상 decoder 버퍼이므로, 이 버퍼에 **새로**
  읽어 들이는 것은 핸드셰이크 구간뿐이다.
- **제안 1안 (선축소, 권장)**: 초기 크기를 핸드셰이크(HELLO greeting)
  최대 크기 수준(수백 B)으로 줄인다. raw 엔진처럼 plug에서 바로 decoder를
  만드는 경로는 이 버퍼를 아예 만들지 않는다. 해제가 아니라 처음부터 작게
  잡는 방식이라, 아래 2안의 수명 문제와 glibc 반환 문제를 모두 피한다.
- **제안 2안 (사후 해제)**: 핸드셰이크 후 `shrink_to_fit()`으로 반환한다.
  단 해제 시점을 "decoder 생성 직후"로 잡으면 안 된다 — greeting과 첫
  데이터 프레임이 한 TCP 세그먼트로 붙어 오면 잔여 바이트(`_insize > 0`)가
  아직 read_buffer 안을 가리키고, backpressure로 입력이 멈춘 경우
  `restart_input()`이 나중에 그 바이트를 다시 읽는다. 해제는 **핸드셰이크
  잔여 입력이 소진된 뒤**에만 안전하다. 또한 8 KB는 glibc mmap 임계
  미만이라 free해도 OS에 바로 반환되지 않고 arena에 남는다(후속 할당에
  재사용되므로 연결이 계속 생기는 서버에서는 기울기 절감으로 나타난다).
- **효과**: 연결당 커밋 −8 KB. 5만 연결이면 −400 MB.
- **성능 위험**: 데이터 경로 성능에는 영향이 없다(핸드셰이크 구간 전용
  버퍼). 대신 2안에는 위의 잔여 입력 수명이라는 정확성 조건이 있으므로,
  구현 시 (a) greeting+데이터 프레임이 붙어 오는 경우, (b) 그 구간에서
  HWM backpressure가 걸리는 경우를 검증 시나리오에 포함해야 한다.

### 6.2 [P2] pipe 청크 크기를 auto-HWM 예산에 맞춤 — 연결당 idle −6 KB, 부하 시 −25 KB+

- **현상**: `message_pipe_granularity = 256`은 소수의 고처리량 연결을
  가정한 값이다. 연결이 수천 개로 늘면 auto-HWM bucket이 pipe당 예산을
  16~32 메시지로 줄이는데, 청크는 여전히 256칸(16.4 KB)이라 낭비다.
  연결마다 청크 2개가 생성 즉시 할당된다.
- **실험 근거 (이번 실측)**: granularity를 16으로 바꾼 실험 빌드에서
  - idle 연결당 RSS 35.6 → **29.6 KB (−17 %)**, 8,000연결 서버 −48 MB
  - SpotNode ALL peer당 105.7 → **82.9 KB (−22 %)**
  - 버스트 중 최고점(VmHWM) 637 → 559 MB (−12 %)
  - 처리량 probe(단일 연결 2M msgs, 64 B / 1 KB): 회귀 미검출
    (측정 노이즈 범위 내, 단 probe 자체 분산이 커서 공식 perf 스위트
    검증이 반드시 필요)
- **제안 (단계적)**:
  1. 단기: 연결(세션↔소켓) pipe에 한해 granularity를 64로 축소
     (청크 4,112 B = 1페이지 + 16 B). 256이 필요한 곳(inproc 고처리량
     등)과 분리하려면 `ypipe_t`의 템플릿 인자를 pipe 용도별로 다르게 준다.
  2. 중기: pipepair 시점에 유효 HWM을 알고 있으므로
     `chunk = min(256, next_pow2(hwm))`처럼 HWM 연동 크기를 선택한다.
     auto-HWM bucket이 줄어드는 대규모 연결일수록 자동으로 작아진다.
- **구현 범위와 설계 결정 두 가지**:
  - 청크 크기는 현재 `ypipe_t<msg_t, N>`의 **컴파일 타임 템플릿 인자**다.
    2안(HWM 연동)은 런타임 가변 청크 yqueue 또는 크기별 다중 인스턴스화
    라는 구조 변경을 뜻하며, 가변화 자체의 핫패스 비용(간접 참조·용량
    필드)을 따로 벤치해야 한다.
  - auto-HWM은 bucket 전환 시 **살아 있는 pipe에도 HWM을 다시 적용**한다.
    청크를 생성 시점 HWM으로 고정하면 이후 HWM이 줄 때는 과대(무해),
    hysteresis로 peer가 줄어 HWM이 다시 커질 때는 과소가 되어 깊은 큐에서
    청크 churn이 영구화될 수 있다. 정책을 명시해야 한다 — 예: 기존 pipe는
    청크 크기를 유지하고 신규 pipe만 새 크기를 쓰거나, HWM 상향 시 churn을
    감수하되 spare-chunk 캐시로 완충한다.
- **성능 위험**: 청크가 작아지면 큐가 깊어질 때 청크 할당/해제가 잦아진다.
  yqueue의 spare-chunk 캐시가 정상 흐름을 흡수하지만, THROUGHPUT profile
  (HWM 512)에서 깊은 큐를 쓰는 워크로드는 회귀 가능성이 있다. 이번
  granularity=16 실험은 전역 상수 변경이라 연결 pipe 외의 ypipe에도
  적용된 상태의 측정이므로, −17 %의 연결 pipe 귀속은 근사치다.

### 6.3 [P3] 코덱 버퍼의 지연 할당과 profile 연동 — 연결당 최대 −20 KB

- **현상**: 핸드셰이크가 끝나면 수신 여부와 무관하게 decoder 16.2 KB,
  송신 여부와 무관하게 encoder 8.2 KB를 할당한다. `in/out_batch_size`는
  공개 옵션이 없는 내부 필드다 (기본 8,192, STREAM 타입만 내부에서
  4,160/4,096으로 덮어쓴다 — §1.2).
- **제안**:
  1. encoder를 첫 사용 시점으로 지연 할당한다. 단, "첫 사용"은 사용자
     송신만이 아니다 — heartbeat PING/PONG(`produce_ping/pong_message`)과
     SUB의 구독 프레임 송신도 encoder를 거치고, `prepare_output_buffer()`는
     encoder가 NULL이면 assert로 죽는다. 따라서 이 경로들 전부에서
     할당을 트리거하도록 NULL-encoder 불변식을 재설계해야 하며, 절감이
     실제로 성립하는 대상은 **heartbeat를 켜지 않은 수신 전용 연결**이다.
     참고로 raw decoder도 지연 할당이 아니다 — proactor 엔진은 plug 직후
     수신을 걸면서 버퍼를 확보하므로(§1.2), 수신 buffer의 진짜 지연
     할당은 별도 설계가 필요하다.
  2. `in/out_batch_size`를 auto-HWM profile에 연동한다. COMPACT profile은
     2,048 B 배치(→ decoder 4.1 KB, encoder 2 KB)로 줄인다. STREAM 타입이
     이미 내부에서 배치를 절반으로 줄여 쓰고 있는 것과 같은 방향이다.
     대량 연결 서버는 연결당 처리량이 낮으므로 배치 축소의 처리량 영향이
     작다.
  3. (선택) 장기 idle 연결의 코덱 버퍼를 해제하고 활동 재개 시 재할당하는
     shrink-on-idle. 구현 복잡도가 있어 1·2 적용 후에도 부족할 때만.
- **성능 위험**: 배치 축소는 대형 메시지 수신 시 read 횟수를 늘린다.
  profile 연동으로 사용자가 선택하게 하면 기본 동작은 그대로다. 1안은
  성능보다 정확성(위 encoder 트리거 누락 시 crash) 검증이 관건이다.

### 6.4 [P4] options_t 사본 축소 — 연결당 −1.9 KB

- **현상**: `options_t`(이 빌드 실측 `sizeof` = 936 B, TLS 필드 포함)가
  연결마다 session과 engine에 각각 복사된다 — 인라인만 연결당 ~1.9 KB.
  내용은 연결 수명 동안 사실상 불변이다.
- **제안**: 소켓이 보유한 불변 스냅샷을 `shared_ptr`로 공유하거나, 엔진이
  실제로 쓰는 스칼라 몇 개만 뽑아 갖는다. `routing_id[256]` 인라인 배열이
  가장 크므로 blob으로 바꾸면 사본당 250 B가 더 준다.
- **성능 위험**: 없음(읽기 전용 공유). 다만 옵션 변경 시점 semantics를
  건드리지 않도록 스냅샷 시점을 현재의 복사 시점과 동일하게 유지해야 한다.

### 6.5 [P5] SpotNode peer당 연결 수 줄이기 — peer당 −35~70 KB

- **단기 (운영 선택)**: 필요 기능만 켠 모드를 쓴다. 실측대로 ALL(3연결,
  105.7 KB/peer) 대신 PUBSUB(2연결, 64.3 KB/peer)이면 peer당 −39 %다.
  routed만 쓰는 배치는 ROUTED 모드로 같은 효과를 본다.
- **중기 (설계 후보)**: peer_ctrl(PUB/SUB) 링크를 별도 TCP로 두지 않고
  기존 데이터 링크(mesh 또는 routed) 위에 컨트롤 프레임으로 다중화하면
  peer당 TCP 1연결을 없앨 수 있다 (peer당 −35 KB + fd −1 + 커널 slab).
  단 다음 세 가지가 설계 전제 조건이다.
  - 컨트롤 프레임이 데이터 pipe의 HWM backpressure와 head-of-line 지연을
    상속한다. 부하 시 peer 생존/멤버십 신호가 늦어져 거짓 peer-down
    판정으로 이어질 수 있으므로, 컨트롤 우선 레인(별도 pipe 또는 우선순위)
    이 필요하다.
  - mesh(PUB/XSUB) 링크는 pipe가 가득 차면 조용히 드랍하는 의미론이라
    그 위에 얹힌 컨트롤 프레임이 유실될 수 있다. 유실 불가 프레임 규정이
    필요하다.
  - 롤링 업그레이드 중 신·구 노드가 섞이므로 프레이밍 버전 협상이
    필요하다.
  컨트롤 프레임 규격 변경이므로 스펙/호환성 검토가 필요한 설계 항목이다.
- **peer 테이블**: peer당 map/set 항목 10~15개(1~2 KB)는 규모에 비해
  작으므로 우선순위가 낮다.

### 6.6 [P6] 커널·운영 가이드 (코드 변경 없음)

- idle 연결의 커널 비용은 작다(§4.3). 수만 연결에서 신경 쓸 것은
  **지속 부하 시** `net.ipv4.tcp_mem`(전역 페이지 예산)과
  `tcp_rmem/tcp_wmem` 최대값이다. 연결이 아주 많고 연결당 대역이 낮은
  배치에서는 `ZLINK_OPT_RCVBUF/SNDBUF`로 소켓당 상한(예: 32 KB)을 정해
  autotuning의 과대 성장(최대 16 MB/소켓)을 막을 수 있다.
- fd 예산: SpotNode ALL mesh는 peer당 3 fd이므로 `RLIMIT_NOFILE`을 연결
  수 기준으로 계산한다. `ZLINK_MAX_SOCKETS`는 연결 수가 아니라 소켓 핸들
  수 제한이므로, 소켓을 많이 만드는 쪽(연결당 소켓 1개인 클라이언트,
  SpotNode 다수 생성 프로세스)에서만 올린다.

### 6.7 [P7] auto-HWM 후속 설계 항목 — 부하 시 최악 경로 닫기

auto-HWM의 뼈대(cap 방식 + 연결 수 bucket + hysteresis)는 유지할 가치가
충분하다. 이 항목은 idle 메모리가 아니라 **부하 시 상한의 정확성**을
보강하는 후속 설계 후보다. P1~P4와 달리 정책 semantics를 바꾸므로
별도 설계/리뷰 트랙으로 분리한다.

1. **count 기반 집행의 바이트 보정.** 예산 계산은 `slot × msg_unit(4 KB)`
   가정인데, 집행은 메시지 개수 기준이다. 16 slot 예산 pipe에 1 MB
   메시지가 쌓이면 실제 노출은 64 KB가 아니라 16 MB가 된다. `maxmsgsize`
   기본값이 -1(무제한)이라 방어선도 없다. 보정 방법 후보:
   - pipe HWM을 메시지 수와 바이트 양쪽으로 집행 (둘 중 먼저 닿는 쪽)
   - 또는 msg_unit보다 큰 메시지를 `ceil(size / msg_unit)` slot으로 계산
   어느 쪽이든 "예산 = 실제 바이트 상한"이라는 의미가 정확해진다.

2. **context/노드 단위 전역 바이트 백스톱.** bucket은 pipe당 예산의
   기울기를 줄일 뿐 총량은 여전히 O(peer 수)다. 최저 bucket(BALANCED
   기준 16 slot × 4 KB = 64 KB/pipe)이어도 5만 peer가 동시에 적체되면
   이론상 ~3.2 GB까지 열려 있다. 현실 워크로드에서 전체 pipe가 동시에
   가득 차기는 어렵지만, 커널이 소켓별 rmem/wmem과 별개로 전역
   `tcp_mem`을 두는 것과 같은 이유로 context(또는 SpotNode) 단위 총
   in-flight 바이트 예산 한 겹이 있으면 최악 경로가 닫힌다. 전역 예산에
   닿았을 때의 동작은 기존 admission 오류 의미(EAGAIN/ENOMEM 계열)를
   재사용한다.

- **적용 범위와 소켓 유형별 의미론 (설계 시 확정 필요)**:
  - conflate pipe는 큐 깊이가 1이라 count/byte 집행 대상이 아니다 —
    명시적으로 제외한다.
  - inproc pipe는 커널 `tcp_mem` 유비가 성립하지 않는 영역이다. 전역
    백스톱의 범위를 원격 전송 pipe로 한정할지 결정해야, 프로세스 내부
    메시징이 원격 적체 때문에 막히는 역전이 생기지 않는다.
  - "기존 admission 오류 의미 재사용"은 블록/에러형 경로(routed send,
    publish admission)에만 정의된다. PUB/XPUB처럼 pipe가 가득 차면 그
    구독자만 조용히 드랍하고 send는 성공하는 소켓에서는, 전역 예산 도달
    시의 동작(드랍 유지 vs 오류)이 공개 계약 변경이 될 수 있으므로 소켓
    유형별(드랍형/블록형/에러형) 동작 표를 설계 단계에서 확정한다.
- **성능 위험**: 1은 pipe 집행 경로에 바이트 누적 카운터가 추가된다
  (원자 연산 1~2개 수준, 벤치 필요). 2는 전역 카운터 경합이 생기지 않게
  socket/pipe 단위로 예산을 나눠 임대(lease)하는 방식이 안전하다.

### 6.8 적용 시 기대치와 P1+P2 실측 결과

사전 기대치:

| 상태 | 현재 | P1+P2 적용 | P1~P4 적용 |
|------|------|-----------|------------|
| idle 연결당 RSS | ~35.6 KB | ~21~24 KB | **~19~22 KB** |
| 지속 부하 연결당 (할당 기반 상한 추정) | ~75 KB | ~40 KB | **~30 KB** |
| 5만 연결 idle 총량 | ~1.78 GB | ~1.1 GB | **~1.0 GB** |
| 5만 연결 부하 총량 (상한 추정) | ~3.7 GB | ~2.0 GB | **~1.5 GB** |

**P1+P2 구현 후 실측 (2026-07-10, 10,000연결까지, 격리 빌드 A/B)**:

P1은 1안(선축소)으로 구현했다 — 생성자 8 KB 할당을 없애고 핸드셰이크
첫 read에서 512 B(`handshake_read_buffer_size`)를 지연 확보한다. raw
엔진은 plug에서 decoder를 먼저 만들므로 이 버퍼를 아예 만들지 않는다.
P2는 1안(고정 축소)으로 구현했다 — 세션↔소켓 pipe만
`session_pipe_granularity = 64`(청크 4,112 B), inproc pipe는 256 유지.
pipe가 자신의 granularity를 기억해 `hiccup()`(재연결) 시에도 같은 청크
크기로 inpipe를 재생성한다(리뷰에서 발견된 재연결 시 256 복귀 누수를
막는다).

패턴별 상세는 §4.1(idle)과 §4.2(잔류/최대)에 통합했다. 핵심 요약:

| 항목 | baseline | P1+P2 | 변화 |
|------|----------|-------|------|
| ROUTER idle 연결당 RSS | 35.6 KB | **28.0 KB** | **−21 %** |
| STREAM idle 연결당 RSS | 34.4 KB | **26.5 KB** | −23 % |
| PUB idle 연결당 RSS | 41.2 KB | **31.2 KB** | −24 % |
| 10,000연결 idle 총 RSS (ROUTER) | 365.1 MB | **289.3 MB** | −75.8 MB |
| 10,000연결 버스트 후 (ROUTER) | 432.4 MB | 340.2 MB | −92 MB |
| 버스트 중 최고점 (ROUTER VmHWM) | 763.8 MB | 665.4 MB | −13 % |
| SpotNode ALL peer당 | 105.7 KB | **78.6 KB** | −26 % |
| SpotNode PUBSUB peer당 | 64.3 KB | **45.7 KB** | −29 % |

실측 절감(−7.6 KB/conn)이 기대치(−14 KB)보다 작은 이유: P1의 −8 KB 중
페이지 공유·allocator 패킹으로 회수되는 커밋은 ~4-6 KB이고, P2의 idle
효과는 첫 페이지 touch 기준이라 청크가 작아져도 커밋 감소는 패킹 개선
분에 그친다. 대신 부하 시(버스트 후 −92 MB, 최고점 −98 MB)에는 청크
상한 축소 효과가 그대로 나타난다.

성능(tcp/1024, `bindings/c/perf` 4패턴 × 3회, 같은 시점 back-to-back):

| 패턴 | baseline (msg/s) | P1+P2 (msg/s) | Δ |
|------|------------------|----------------|----|
| DEALER_ROUTER | 1,226,794 | 1,199,333 | −2.2 % |
| DEALER_ROUTER_REQREP | 159,784 | 162,213 | +1.5 % |
| PUBSUB | 760,288 | 806,487 | +6.1 % |
| SPOT | 407,841 | 418,002 | +2.5 % |

run 간 자체 노이즈가 ±2~6 %인 환경(WSL2)이라 모두 노이즈 범위 내 —
**회귀 미검출**. 전 패턴·전 크기 공식 perf 스위트는 커밋 전에 별도
실행이 필요하다. 기능 테스트 8종(recv_part, zmp_request_reply,
heartbeats, stream_socket, backpressure_matrix, spot_pubsub_scenario,
zmp_decoder, ctx_destroy)은 전부 통과했다. `test_ctx_destroy`의 inproc
pending disconnect 케이스는 P1·P2와 무관한 별개 결함이었고,
disconnect 시 pending inproc 연결을 임시 PAIR 소켓으로
materialize(`ctx_t::materialize_pending_inproc`)하는 수정으로 함께
해결했다. 코드 리뷰(4각도)에서 나온 지적 중 hiccup 재연결 시 청크가
256으로 복귀하는 문제와 materialize 실패 시 disconnect가 정리를
건너뛰는 회귀도 반영했다.

## 7. 재현 방법

```bash
# 1) core 라이브러리를 먼저 빌드해 둔다 (core/build.sh)

# 2) study 빌드
cd core/study
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 3) 전체 측정 (router/stream/pub/spot_all/spot_pubsub)
python3 run_study.py            # results/<scenario>.json + results/summary.md
python3 run_study.py --quick    # 빠른 스모크

# 4) 처리량 probe (실험 빌드 비교용)
python3 run_perf_probe.py --label baseline
LD_PRELOAD=<실험 libzlink.so> python3 run_perf_probe.py --label exp
```

측정 원리: 서버 프로세스가 stdin 명령(`rss`, `trim`, `count`, `status`)을
받아 각 단계에서 `malloc_trim(0)` 후 `/proc/self/status`(VmRSS/VmHWM)와
`/proc/self/smaps_rollup`(Anonymous/Private)을 출력한다. 클라이언트는
별도 프로세스라 서버 RSS를 오염시키지 않는다. 연결당 수치는 연결 수
구간(1,000→8,000 등)의 RSS 기울기다.

주의:
- WSL2 환경 수치다. 실서버(베어메탈/일반 VM)에서 절대값은 다를 수 있으나
  구성비와 기울기는 코드 구조에서 나오므로 유효하다.
- 모든 수치는 구성당 1회 실행이고, 연결당 기울기는 회귀선이 아니라 구간
  양 끝점 기준이다. 절대값을 정밀하게 쓰려면 시나리오당 3회 이상 반복해
  평균±편차를 병기하고 전체 단계 최소자승 기울기로 계산하는 것이 좋다.
- `malloc_trim(0)` 직후의 RSS라서 **바닥값**이다. 운영 프로세스는 trim을
  호출하지 않으므로 실제 RSS는 glibc arena가 쥐고 있는 해제 페이지만큼
  더 높다. 할당자 교란을 정량화하려면 `MALLOC_ARENA_MAX=1` 대조 실행이나
  jemalloc 교차 측정을 권한다.
- `run_perf_probe.py`는 노이즈가 큰 sanity check다(단일 연결, blocking
  send, 0.5초 폴링 양자화). §6 제안을 실제로 적용할 때는 저장소의 공식
  perf 벤치로 baseline vs patched를 비교한 뒤 커밋해야 한다.

## 부록 A. 원본 측정 데이터

- `results_baseline/` — 초기 탐색 측정, 8,000연결까지 (json + summary.md)
- `results_g16/` — `message_pipe_granularity=16` 실험 빌드 (router, spot_all)
- `results_ab_base/`, `results_ab_patch/` — P1+P2 A/B (router, spot_all),
  10,000연결 (`ab_base.log`, `ab_patch.log`)
- `results_fill_base/`, `results_fill_patched/` — stream/pub/spot_pubsub
  A/B, 10,000연결 (`fill_base.log`, `fill_patched.log`) — §4.1~4.2의 근거
- `results_full*.log` — 실행 로그 원본
- perf A/B 결과: `bindings/c/perf/results/single/report/`의
  `*_memstudy-base.txt`, `*_ab-base.txt`, `*_ab-patch.txt`

실험 빌드는 `src/runtime/utils/config.hpp`의 `message_pipe_granularity`만
바꿔 별도 디렉토리(`core/build_memexp`)에서 빌드했고, 소스는 원복했다.
