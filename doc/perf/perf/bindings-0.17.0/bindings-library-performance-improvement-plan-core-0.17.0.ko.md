# Core 0.17.0 bindings 라이브러리 성능 개선 계획

> 시작일: 2026-09-05
>
> 작업 기준: `main` (별도 브랜치 없음; 검증된 단위마다 커밋·푸시)
>
> 현재 상태(2026-09-07 갱신): 러너 정합 작업 중. 호스트가 바뀌었고(16 논리 CPU / 11.7 GiB /
> 홈 `/home/hep7hep7` → 20 논리 CPU / 94 GiB / 홈 `/home/hep7`) `core/build`(Release+LTO)를
> 새로 빌드해 artifact를 고정했으며 새 환경 manifest(`log/2026-09-07-environment.ko.md`)를
> 작성했다. **2026-09-05/06의 paired 판정과 개선 pass는 그대로 유지한다** — 같은 호스트·같은
> 시점에 C와 짝지어 잰 결과이므로 그 자체로 유효하다. §6이 금지하는 것은 "이전 C 결과와 새
> binding 결과를 짝짓는 것"이지 완결된 판정의 폐기가 아니다. 새로 손대는 대상만 새 artifact에서
> 다시 짝지어 재고, 이미 닫힌 판정은 대표 셀 spot-check로 확인한다. 측정 재개 전에 러너의 정책
> 정합을 먼저 끝낸다(D-BP1·D-BP2·D-BP6).
>
> 사용자 지시(2026-09-07): 모든 언어의 perf 러너는 `bindings/c/perf` canonical 러너와
> **완전히 동일한 측정 의미**를 가져야 하고 `doc/perf`의 정책 문서(`PERF_POLICY.md`,
> `PERF_SINGLE_TEST_POLICY.md`, `PERF_MULTI_TEST_POLICY.md`,
> `BINDINGS_OPTIMIZATION_GUIDE.ko.md`)를 준수해야 한다. REQREP뿐 아니라 one-way·SENDSEND·
> STREAM을 포함한 모든 pattern에서 C↔binding 측정 의미 차이를 제거한 뒤 측정한다.
>
> 다만 **C perf는 동기 모델이고 나머지 binding perf는 비동기로 동작하는 차이는 어쩔 수 없으며,
> 그렇게 동작하도록 테스트한다(특히 multi)**. 정합의 범위는 둘로 나눈다. (가) 반드시 같아야
> 하는 것 = 측정 의미: metric 산출식과 runs median, `ready -> active(duration)` 구간과 같은
> 구간 latency 집계, **backpressure 경계까지 포화 제출이라는 부하 수준**, latency sample
> cap·reservoir·percentile 보간, 수신 readiness/drain 의미, 종료 protocol, client·I/O
> thread·HWM·timeout·size·duration. (나) 같게 강제하지 않는 것 = 언어 실행 모델: completion
> queue·callback·future·event loop·goroutine 등 binding 공개 계약의 비동기 경로 자체.
> 동기처럼 보이게 하려고 요청마다 blocking wait을 넣거나 공개 계약을 우회하는 러너 설계는
> 채택하지 않는다. 목표는 "같은 코드 모양"이 아니라 "같은 부하와 같은 집계"다. socket당
> in-flight 1로 묶는 현재 REQREP 러너 구조는 비동기/동기 차이가 아니라 부하 수준 차이이므로
> (가)에 속하는 수정 대상이다(D-BP1).
>
> D-B127 사용자 결정(D-BP1): (B1) binding 러너의 "backpressure까지 포화 제출" 경계는
> 공개 poller의 raw socket `ZLINK_POLLOUT` level로 정의하고 새 공개 API를 추가하지 않는다.
> (B2) canonical C single 러너의 별도 1초 latency 단계를 제거해 정책대로 throughput과
> latency를 같은 active 구간에서 집계한다.
>
> 이전 상태(2026-09-05 기록): inventory gate와 재현 환경 기록 완료, paired 측정 대기. Core
> REQUEST 계약 통일(D-B85), 8개 binding의 REQUEST 경로 포팅, C perf REQREP runner 반영 확인.

이 문서는 Core 0.17.0을 기준으로 bindings 라이브러리 성능 개선을 처음부터 진행하기
위한 실행 문서다. 이전 계획 문서(0.15.0)의 측정값과 완료 판정은 가져오지 않는다. 새 C
기준 결과와 각 binding의 새 결과만 이 문서에 기록한다. 이 계획서에는 측정 대상, 측정
조건, report 경로, 비교값과 판정만 남긴다. 실행 명령, 후보 검토, 프로파일과 같은 과정
설명은 이 문서가 있는 폴더의 `log/`에 기록한다.

0.17.0에서 바뀐 전제:

- DONTWAIT send와 request는 admission을 한 번만 시도하고, 거절되면
  `ZLINK_SUBMIT_BACKPRESSURED`와 대기 토큰을 돌려준다. credit이 돌아오면
  `ZLINK_COMPLETION_WRITABLE`이 completion queue에 들어오고 `ZLINK_POLLOUT`이 level로
  유지된다. 애플리케이션(binding)은 queue를 `NO_DATA`까지 비운 뒤 같은 record를 다시
  제출한다. Core는 payload를 보관하지 않으며 REQUEST pending pool도 없다
  (`core/doc/spec/core/socket/README.ko.md` "Part send" 절, 결정 D-B79·D-B85).
- 따라서 binding의 hot path는 "즉시 성공 경로"와 "거절 → 토큰 대기 → 재제출 경로"로 나뉜다.
  즉시 성공 경로에는 완료 객체·map 등록·payload 복사·poller 생성이 없어야 하고, 거절
  경로는 실제 wake 소스(public poller 또는 runtime owner)에서만 깨어나야 한다. 이미 확인된
  기법과 기각 목록은 [BINDINGS_OPTIMIZATION_GUIDE.ko.md](../../BINDINGS_OPTIMIZATION_GUIDE.ko.md)를
  따른다. 2026-09-04 리뷰에서 각 binding에 적용된 수정과 전/후 수치는 그 가이드 §3에 있으며,
  이 문서의 통과 비율이나 완료 근거로 사용하지 않는다.
- C perf runner는 정책 §1.2·§5.1대로 in-flight 상한 없는 단일 활성 phase로 측정한다
  (`bindings/c/perf`, 커밋 00a668fe90). binding runner도 같은 모델이어야 하며, 다른 모델의
  runner로 얻은 값은 비교에 쓰지 않는다.
- Core 0.17.0 자체의 0.15.1 대비 위치는 별도 문서(`doc/plan/c016-worklog/decisions.ko.md`
  D-B82~D-B84)에 있다. 이 계획은 같은 Core 0.17.0 위에서 C 대비 binding 비율만 판정한다.

## 1. 기준 버전과 시작 상태

> **공식 측정 runtime은 Core 0.17.1이다 (D-BP5).** 사용자가 0.17.1 릴리스를 통보하면 그
> runtime으로 artifact를 재고정하고 새 환경 manifest를 쓴 뒤 측정을 시작한다. 현재 고정한
> 0.17.0 local artifact(`core/build/lib/libzlink.so.0.17.0`, Build ID `af759a1c…`)는 러너
> 정합 작업의 smoke·검증 전용이며, 이 artifact로 얻은 수치는 기준값이나 판정 근거로 쓰지
> 않는다. 아래 0.17.0 기준 서술은 그 전제로 읽는다.

이번 작업의 core 기준 버전은 0.17.0이다. 측정 전에 다음 세 파일의 버전이 모두 같은지
확인한다.

- `VERSION`: `LIBZLINK_VERSION=0.17.0`
- `core/CMakeLists.txt`: `project(zlink VERSION 0.17.0 ...)`
- `core/include/zlink.h`: major, minor, patch values matching 0.17.0

`core/v0.17.0` release asset이 발행되기 전까지 공식 측정 runtime은 이 작업영역의
`core/build`(`scripts/build-core.sh release`, Release+LTO)에서 만든 `libzlink.so.0.17.0`이다.
모든 perf runner에 `ZLINK_CORE_SOURCE=local`을 명시하고, runner가 출력하는
`Verified benchmark Core runtime: .../libzlink.so.0.17.0`과 binding의 public version API가
0.17.0을 보고하는지 확인한다. release asset이 발행되면 그 시점부터 `--core-version 0.17.0`
release runtime으로 전환하고, 전환 전후의 C 기준값을 한 번 다시 재어 같은지 확인한다.
측정 도중 `core/build`를 다시 build하지 않는다(Core 변경이 필요하면 측정을 멈추고 새 기준값부터
다시 잰다). 다른 버전의 local package나 오래된 runtime을 사용한 결과는 이 문서의 기준값으로
사용하지 않는다.

모든 성능 셀은 `미측정`에서 시작한다. 상세 표에는 현재 binding runner에 실제로 등록된
pattern만 포함한다. 공식 C runner에만 있고 binding runner에 없는 pattern은 이 계획의
측정 대상에서 제외한다. 이전 문서와 이전 report는 병목 후보를 찾는 참고 자료로만 사용하며,
core 0.17.0의 통과 비율이나 완료 근거로 사용하지 않는다.

언어별 pattern 목록이 다른 것은 Core C API 또는 binding public contract가 언어별로 다르다는
뜻이 아니다. 모든 binding은 같은 Core C API를 감싸지만, 각 언어의 perf runner가 현재
구현하고 등록한 측정 scenario가 다를 수 있다. 따라서 이 문서의 언어별 차이는 public API
차이가 아니라 perf runner 구현 범위의 차이로 해석한다.

## 2. 범위와 목표

개선 대상은 perf 코드가 아니라 다음 bindings 라이브러리다.

| 순서 | 언어 | perf 경로 | 우선순위 |
|------|------|-----------|----------|
| 1 | C++ | `bindings/cpp/perf` | **우선** |
| 2 | .NET | `bindings/dotnet/perf` | **우선** |
| 3 | Java | `bindings/java/perf` | **우선** |
| 4 | Node | `bindings/node/perf` | **우선** |
| 5 | Go | `bindings/go/perf` | 후순위 |
| 6 | Rust | `bindings/rust/perf` | 후순위 |
| 7 | Python | `bindings/python/perf` | 후순위 |

우선 개선 대상은 C++·.NET·JVM·Node 4개다(D-BP8). Go·Rust·Python은 앞 4개가 끝난 뒤
착수한다. 후순위 언어의 러너 정책 위반 기록은 유지하며, 그 언어를 측정할 때 위반을
먼저 고친 뒤 잰다.

> **비교 범위 (2026-09-07, D-BP2)**: `PERF_POLICY.md` line 144~148은 "bindings ↔ C 성능
> 비교는 multi suite로 한정한다 ... single 결과는 각 binding의 synchronous path와 lifecycle을
> 검증하는 데 사용하고 cross-binding ratio에는 사용하지 않는다"고 규정한다. 따라서 이 문서의
> **통과/보류/미달 판정과 완료 기준은 Multi suite에만 적용한다.** Single suite는 정책 준수와
> lifecycle 검증을 위해 계속 측정·기록하되 C 대비 비율에 gate를 걸지 않는다. §2.1·§2.2의 목표
> 수치, §7.4·§7.5의 완료 gate, §9.x.1 Single 상세 표의 상태 값, §12의 완료 기준을 모두 이
> 범위로 읽는다.

비교 기준은 같은 core 0.17.0 runtime으로 실행한 `bindings/c/perf` 결과다. 같은 suite,
pattern, transport, message size, duration, client 수, metric을 맞춘 뒤 다음 식으로
비율을 계산한다.

```text
binding ratio (%) = binding throughput / C throughput * 100
```

### 2.1 Throughput 목표

> **주의 — 2026-08-23/24 realignment 이전 historical thresholds**
>
> 이 절의 표와 예외 설명에 인용한 historical numbers(과거 p10·하위 25% 경계값·중앙값·
> per-cell exception·ceiling)는 2026-08-23/24 realignment 전에 측정한 기록이다. 당시의
> binding async-admission machinery를 사용했으며, realignment에서 sync submit terminal로
> 정렬하고 해당 machinery를 삭제했으며 send-completion을 Core가 담당하도록 변경했다. 이로
> 인해 routed-path economics가 근본적으로 달라졌고 C++ `DEALER_DEALER / 64B`는 +185%
> 개선됐다.
>
> 이 historical numbers는 초기 target을 seed하는 참고값으로만 사용한다. 새 contract에서
> 각 언어의 첫 paired measurement를 기준으로 그 언어의 per-cell exception과 median을 반드시
> 다시 도출(MUST re-derive)하며, 기존 ceiling은 한계로 취급하지 않는다. 아래 수치는 기록이므로
> 삭제하지 않는다.

각 언어에는 개별 셀의 **최소 기준**과 같은 pattern·transport에 속한 message size 비율의
**목표**를 둔다. transport의 throughput 판정은 모든 측정 size ratio의 **산술평균
(aggregate mean)**을 gate로 사용한다. 개별 size가 최소 기준보다 낮아도 종합 평균이 목표를
충족하면 그 값만으로 전체를 미달로 바꾸지 않는다. 개별 값은 병목 위치와 결과를 확인하기
위한 측정 기록으로 남긴다. 중앙값과 반복값은 보조 비교 자료로 기록한다. 비율 자체에는
상한을 두지 않는다.

작업 순서는 이 aggregate mean을 완료 gate로 사용한다. 현재 비교 대상의 aggregate mean이
해당 pattern 그룹의 목표보다 낮으면 다음 transport·pattern·언어로 이동하지 않는다. binding
hot path를 개선하고 같은 C 기준으로 다시 비교하여 목표를 충족한 뒤에만 다음 항목을 측정한다.
측정 변동, 노트북 환경, 안정성 같은 이유만으로 목표 미달 항목을 보류 또는 완료로 표시하지
않는다.

C++의 단순 one-way 중앙값 목표는 기본 95%다. 이 목표를 맞추기 위한 개선 작업이 과도하게
길어지는 경우에는 현재 작업에서만 90%를 완화 목표로 선택할 수 있으며, 선택 사실과 근거를
결과에 기록한다. 완화 목표를 선택해도 size ratio 산술평균 90%를 달성해야 한다.

`doc/perf/perf/log/`의 과거 측정값은 달성 가능한 범위를 판단하는 참고 자료다. 과거 결과가
완전히 최적화된 상태라고 가정하지 않으며, p10이나 하위 25% 경계값을 목표로 자동 변환하지
않는다. 목표는 C와 동일하게 제거할 수 있는 비용, public binding 계약 때문에 필요한 비용,
GC·JIT·callback·event loop 같은 언어 runtime 비용을 나누어 판단한다. 낮은 과거 값만으로
목표를 낮추지 않고, 반대로 다른 언어의 높은 값을 근거로 달성하기 어려운 목표를 강제하지
않는다.

| 언어 그룹 | Pattern 그룹 | 과거 실측 p10 | 과거 실측 하위 25% 경계값 |
|-----------|--------------|---------------|----------------------------|
| C++ / Rust | 단순 one-way | 89.1% | 95.1% |
| C++ / Rust | routed one-way | 62.5% | 88.2% |
| C++ / Rust | socket request/reply | 88.9% | 92.0% |
| C++ / Rust | multi routed echo | 82.6% | 89.5% |
| .NET / Java | 단순 one-way | 74.9% | 87.6% |
| .NET / Java | routed one-way | 78.1% | 83.6% |
| .NET / Java | multi routed echo | 54.8% | 57.0% |
| Go | 단순 one-way | 59.4% | 68.2% |
| Go | routed one-way | 50.0% | 55.8% |
| Go | multi routed echo | 41.3% | 42.4% |
| Node | 단순 one-way | 33.6% | 36.1% |
| Node | routed one-way | 33.0% | 39.4% |
| Node | multi routed echo | 29.9% | 31.1% |

Python의 과거 full matrix는 이후 공개 계약 복구 전 구현으로 측정한 값이므로 달성 가능성
판단에서도 제외한다. C++ socket request/reply의 완료 셀은 p10 88.9%, 중앙값
96.3%였고 routed one-way와 multi routed echo도 비슷했다. 다만 현재 core 0.17.0의
`MULTI_ROUTER_ROUTER_REQREP / ws`를 공개 callback 계약으로 반복 측정한 결과, 제거 가능한
vector 경유와 routing id 변환을 없앤 뒤에도 대형 셀은 76.4~78.0%였다. 따라서 C++
socket request/reply는 중앙값 85%를 유지하고 개별 셀 최소 기준만 75%로 둔다. Rust는
별도 언어 목표를 사용하며, 이후 현재 runtime 측정과 개선 결과로 달성 가능성을 다시 확인한다.

| Pattern 그룹 | 포함 pattern |
|--------------|--------------|
| 단순 one-way | `PAIR`, `PUBSUB`, `DEALER_DEALER`, `MULTI_PUBSUB`, `MULTI_STREAM` |
| routed one-way | `DEALER_ROUTER`, `ROUTER_ROUTER` |
| socket request/reply | `DEALER_ROUTER_REQREP`, `ROUTER_ROUTER_REQREP`, `MULTI_DEALER_ROUTER_REQREP`, `MULTI_ROUTER_ROUTER_REQREP` |
| multi routed echo | `MULTI_DEALER_ROUTER_SENDSEND`, `MULTI_ROUTER_ROUTER_SENDSEND` |

아래 값은 `최소 기준 / 중앙값 목표`다. 언어 runtime과 binding 경계를 따로 반영하기 위해
언어를 묶지 않는다.

| 언어 | 단순 one-way | routed one-way | socket request/reply | multi routed echo |
| ------ | --------------- | ---------------- | ---------------------- | ------------------- |
| C++ | 85% / 95% | 80% / 85% | 75% / 85% | 80% / 85% |
| .NET | 64% / 85% | 75% / 80% | 50% / 70% | 50% / 70% |
| Java | 70% / 90% | 75% / 85% | 50% / 70% | 50% / 70% |
| Node | 35% / 60% | 33% / 60% | 30% / 60% | 30% / 60% |
| Go | 55% / 65% | 50% / 57% | 40% / 53% | 40% / 53% |
| Rust | 85% / 95% | 70% / 85% | 70% / 85% | 70% / 85% |
| Python | 35% / 60% | 33% / 60% | 30% / 60% | 30% / 60% |

Node의 과거 size 중앙값은 pattern 그룹에 따라 55.3~91.8%였다. 아직 최적화가 끝난
결과가 아니므로 가장 낮은 값에 맞춰 목표를 낮추지 않고 모든 pattern 그룹의 중앙값 목표를
60%로 둔다. Python의 과거 full matrix는 공개 계약 복구 전 결과이므로 목표를 낮추는
근거로 쓰지 않으며 Node와 같은 60% 중앙값 목표에서 시작한다. 이후 현재 core 0.17.0의
paired 측정과 binding 개선으로 달성 가능성을 검증한다.

.NET의 과거 size 중앙값은 multi routed echo 66.1%였고 Java는 69.8%였다. 이 값도
최적화 한계가 아니므로 request/reply와 multi routed echo의 중앙값 목표를 70%로 둔다.
Java의 단순 one-way와 routed one-way는 과거 중앙값도 각각 98.7%, 112.6%였으므로
90%, 85%를 달성 가능한 중앙값 목표로 사용한다.

.NET `PAIR / tcp / 256B`는 공개 builder 제거 진단에서도 C 대비 65.2%가 상한이었고,
현재 public 경로의 독립 paired 측정 두 번은 64.9%와 64.7%였다. 크기 중앙값은 약
86.5%이므로 중앙값 목표 85%는 유지하고, 단순 one-way의 개별 셀 최소 기준만 64%로
둔다. 한 크기의 runtime 경계 비용 때문에 평균 목표를 낮추지는 않는다.

.NET Single `DEALER_ROUTER / ws / 256B`는 public builder와 routed receive 계약을
유지한 공식 측정과 진단 측정에서 C 대비 69.4~70.7%가 반복됐다. raw native 경로,
builder 재사용, latency 계측 축소는 각각 공개 경로 우회, 수명 계약 훼손, 측정 의미 변경
문제가 있거나 처리량을 개선하지 못했다. routed one-way의 중앙값 목표 80%와 다른 셀의
최소 75%는 유지하고 이 셀에만 최소 69%를 적용한다.

.NET Single `DEALER_ROUTER / ipc / 256B`도 같은 public builder와 routed receive
경로에서 전체 측정 74.0%, 독립 paired 재측정 71.4%였다. 다른 다섯 크기의 비율은
77.3~104.7%이고 크기 중앙값은 약 90.8%이므로 전역 목표를 낮추지 않는다. 이 셀에만
최소 71%를 적용하고 routed one-way 중앙값 목표 80%와 다른 셀의 최소 75%는 유지한다.

.NET Single `ROUTER_ROUTER / ipc / 65536B`도 전체 측정 72.1%, CPU idle 94%에서의
독립 paired 재측정 71.8%로 반복됐다. 다른 다섯 크기는 79.4~99.1%이고 크기 중앙값은
약 87.0%다. raw native send, builder 재사용과 snapshot 수명 변경 없이 제거할 수 있는
비용이 없으므로 이 셀에만 최소 71%를 적용한다. 중앙값 목표 80%와 다른 셀의 최소
75%는 유지한다.

`inproc`은 network와 TLS 비용이 없어 C 기준이 memory copy 상한에 가까워진다. `ipc`도
network 비용이 없고, 256B의 public builder 경계와 1KiB 이상에서 필요한 `Message` snapshot
비용이 C 기준에서 더 크게 드러난다. .NET public `Message`의 snapshot과 managed/native
transition을 제거하면 측정 의미나 안전 계약이 달라지므로, 아래 local transport 예외를
사용한다. 다른 transport의 언어 목표에는 적용하지 않는다.

| 언어 | Transport | Pattern 그룹 | 최소 기준 / 중앙값 목표 |
|------|-----------|--------------|--------------------------|
| .NET | `inproc` | 단순 one-way | 24% / 45% |
| .NET | `ipc` | 단순 one-way | 64% / 82% |
| .NET | `inproc` | `DEALER_ROUTER` | 24% / 60% |
| .NET | `inproc` | `ROUTER_ROUTER` | 24% / 55% |

**재검증 주의:** 아래 `ROUTER_ROUTER / inproc` 특수 사례의 sender가 public routed builder
boundary를 통과한다는 rationale와 54.5%/50.10% 중앙값은 2026-08-23/24 realignment 이전의
builder 비용 구조에 기반한다. realignment 이후에는 이 rationale와 그에 따른 threshold를
재사용하기 전에 새 paired 측정으로 반드시 다시 검증한다.

`ROUTER_ROUTER`는 sender도 public routed builder와 routing metadata 경계를 통과하므로
`DEALER_ROUTER`보다 local memory-copy 상한에서 고정 비용이 더 크게 드러난다. 전체 크기
paired 측정과 저부하 대형 셀 재측정의 중앙값은 약 54.5%와 50.10%였다. pooled snapshot과
block copy 후보도 최종 5회에서 악화돼 제거했으므로 `ROUTER_ROUTER / inproc`에만 중앙값
55%를 적용한다. 개별 셀 최소 24%와 다른 transport의 목표는 유지한다.

`ROUTER_ROUTER` 계열은 절대 기준과 함께 같은 suite와 mode의
`DEALER_ROUTER` 대비 상대 비율도 확인한다. 절대 기준을 통과한 셀은 상대 비율만으로
미달로 바꾸지 않지만, C와 비교해 두 routed pattern 사이의 차이가 지나치게 크면 병목
후보로 기록한다.

### 2.2 Latency 목표

throughput과 같은 방식으로 size별 평균 latency ratio의 중앙값을 aggregate 값으로 계산한다.
이 aggregate latency가 아래 상한을 넘으면 `미달`로 판정한다. 개별 size의 latency ratio가
상한을 넘어도 aggregate latency가 상한 이내이면 그 개별 값만으로 전체를 미달로 바꾸지
않는다. 해당 값은 latency outlier로 기록한다. p95와 p99는 진단 자료로만 기록하고 목표
통과 여부에는 사용하지 않는다.
C의 평균 latency가 0으로 기록된 결과는 유효한 비율을 계산할 수 없으므로
다시 측정한다.

| 언어 그룹 | 평균 latency의 C 대비 최대 비율 |
|-----------|------------------------------------|
| C++ / Rust | 2.0배 |
| .NET / Java / Go | 3.0배 |
| Node / Python | 5.0배 |

같은 timestamp 경계와 평균 계산을 사용해도 C의 평균 latency가 매우 낮은 PUBSUB 셀에서는
managed subscriber가 형성하는 queue 깊이와 고정 수신 비용이 비율을 크게 만든다.
반복 측정과 제거 가능한 binding 비용 검토를 마친 아래 셀에만 별도 상한을 적용한다.
다른 크기, pattern, transport의 언어별 상한은 바꾸지 않는다.

| 언어 | Suite / Pattern | Transport | Message size | 평균 latency의 C 대비 최대 비율 |
|------|-----------------|-----------|--------------|------------------------------------|
| .NET | Single `PUBSUB` | `tls` | 65536B 이상 | 6.0배 |
| .NET | Single `PUBSUB` | `inproc` | 64B | 15.0배 |
| .NET | Single `DEALER_DEALER` | `ws` | 256B | 6.0배 |
| .NET | Single `DEALER_ROUTER` | `ws` | 131072B | 5.0배 |
| .NET | Single `DEALER_ROUTER` | `tls` | 131072B | 3.5배 |

목표 경계 셀과 secure transport는 5회 반복 결과로 판정한다. 최적화 전후를 비교할 때
대상이 아닌 대표 셀의 throughput 중앙값이 5% 넘게 낮아지거나 평균 latency가 10% 넘게
높아지면 회귀로 판정한다.

## 3. 측정 크기

### 3.1 Single suite

Single 기본 크기는 기존 구성을 유지한다.

| 표시 | bytes |
|------|-------|
| 64 B | 64 |
| 256 B | 256 |
| 1 KiB | 1024 |
| 64 KiB | 65536 |
| 128 KiB | 131072 |
| 256 KiB | 262144 |

### 3.2 Multi suite

core 0.17.0의 현재 multi runner 기본값을 따른다. 이전 표의 256 KiB는 제거하고
4 KiB를 추가한다.

| 표시 | bytes | 상태 |
|------|-------|------|
| 64 B | 64 | 측정 |
| 256 B | 256 | 측정 |
| 1 KiB | 1024 | 측정 |
| 4 KiB | 4096 | 새로 추가 |
| 64 KiB | 65536 | 측정 |
| 128 KiB | 131072 | 측정 |

`MULTI_STREAM`의 현재 기본 크기는 64, 256, 1024, 65536 bytes다. 따라서 상세 표에서
`MULTI_STREAM`의 4096과 131072 셀은 `해당 없음`으로 시작한다. runner 정책이
변경되어 이 크기들이 공식 기본 측정 대상이 되면, C와 모든 binding의 조건을 함께 맞춘
뒤 상태를 변경한다.

## 4. 측정 전 inventory gate

성능 측정 전에 각 공식 runner의 `ALL` 범위를 정적 검사한다. pattern, transport,
message size, 기본 client 수와 지원 option을 아래 네 곳에서 대조한다.

1. `bindings/c/perf` runner
2. 각 binding의 공식 runner
3. `doc/perf` 정책 문서
4. 이 문서의 상세 표

하나라도 다르면 해당 pattern의 paired 측정을 시작하지 않는다. 각 binding의 공식 runner에
실제로 등록된 pattern만 이 문서의 상세 표와 paired 측정 대상에 포함한다. 공식 C에만 있고
binding runner에 없는 pattern은 이 계획의 측정 대상에서 제외한다. 새 pattern이나 public API가
필요하면 별도 설계·계약 검토 후 runner와 문서를 함께 갱신한다.

지원하지 않는 CLI option을 runner가 성공으로 받아들인 뒤 무시해서는 안 된다.
실제로 적용하거나 명확한 오류로 거부해야 한다. 현재 .NET single runner의
`--output`, `--pin-cpu`, I/O thread, HWM, buffer, timeout option은 측정 전에
적용 여부를 확인한다. 조건 정렬에 필요한 option이 무시되면 해당 runner의 측정을
시작하지 않는다.

C multi runner의 memory guard가 기본 client 수를 줄였으면 그 결과는 paired 비교에
사용하지 않는다. binding runner에도 같은 종류의 cap이 있으면 동일하게 적용한다. C와
binding report에서 실제 client 수와 STREAM client 수가 같은지, memory guard cap이
발생하지 않았는지 확인한다.

## 5. 고정 원칙

- 성능 목표 달성이 작업의 우선 목적이지만, 개선 설계와 구현은
  `doc/principal/dev/posddd.ko.md`의 POSDDD 원칙을 계속 만족해야 한다.
- 각 후보는 성능 변화와 별도로 POSDDD 설계 이득을 평가한다. 성능 개선이 없거나
  작더라도 처리량·평균 latency·기능 회귀가 없고 정보 은닉, 책임 경계, 중복 제거 또는
  hot path의 불필요한 특수 경우를 명확히 개선하면 최종 코드로 채택할 수 있다. 이 경우
  성능 목표를 통과한 것으로 바꾸지 않고, 측정 결과와 POSDDD 채택 근거를 함께 기록한다.
- 성능 개선은 각 binding의 public API를 사용하는 일반 경로에서 이루어져야 한다.
- contract의 public interface(공개 함수·메서드 signature, 공개 type·enum 값,
  ownership·error 동작)는 변경하지 않는다. 기존 public interface의 변경도 허용하지
  않으며, 성능 개선은 현재 interface를 호출하는 binding 내부 구현과 perf harness의
  의미 정렬 범위에서만 수행한다.
- perf 전용 public API, private API 접근, C API 직접 호출, 특정 입력만 겨냥한 우회는
  개선으로 인정하지 않는다.
- perf는 측정 의미가 C와 다르거나, 실제 버그가 있거나, `doc/perf` 정책을 위반한
  경우에만 수정한다.
- binding에 C와 같은 pattern이 없으면 같은 측정 의미로 binding perf만 추가한다. 성능
  수치를 유리하게 만들기 위해 확정된 C perf, sampler, HWM, timeout, sleep,
  측정 흐름을 바꾸지 않는다.
- 새 helper나 공개 API를 만들기 전에 기존 public API와 내부 구현으로 해결할 수 있는지
  먼저 확인한다.
- allocation, copy, dispatch, callback, poller, ownership, error 처리 비용은 호출자에게
  새 설정이나 실행 순서를 요구하지 않고 binding 내부에서 줄인다.
- timeout 증가, sleep 추가, retry 반복, client 수 축소로 실패를 숨기지 않는다.
- Core 버그이면 이 작업에서 source를 다시 build해 측정 runtime을 바꾸지 않는다. 별도
  Core release와 version을 확정하고, 새 release package provenance를 기록한 뒤 C와
  binding의 paired 기준을 다시 만든다.
- perf 결과는 report가 `status: complete`일 때만 표에 반영한다. 중단되었거나 일부
  RESULT만 생성된 report는 근거로 사용하지 않는다.
- `doc/perf/PERF_POLICY.md`, `doc/perf/PERF_SINGLE_TEST_POLICY.md`,
  `doc/perf/PERF_MULTI_TEST_POLICY.md`를 따른다.

## 6. 재현 환경 기록

C와 binding을 paired 측정할 때 같은 session tag를 사용하고 다음 정보를 이 문서가 있는
폴더의 `log/`에 기록한다. 계획서의 결과 표에는 비교에 필요한 조건과 결과만 요약한다.

| 항목 | 기록 내용 |
|------|-----------|
| source | git commit, dirty 여부, 변경 파일 목록 |
| core | release 버전과 tag, runtime 절대 경로, package provenance |
| binding | package 버전, compiler 또는 runtime 버전 |
| host | OS, kernel, CPU model, 논리 CPU 수, memory |
| CPU 상태 | governor, CPU pinning, 측정 중 다른 고부하 작업 유무 |
| 명령 | C와 binding에 사용한 전체 명령과 성능 관련 환경 변수 |
| 조건 | suite, pattern, transport, size, duration, runs, client 수, I/O thread 수 |
| 결과 | report 경로, `status`, Effective Options, auto-HWM detail |
| pair | C와 binding에 공통으로 부여한 session tag |

`auto-HWM` 기본값은 0.13.0에서 프로파일별 비율 2%/3%/5%/8%와 고정 cap 64/256/512/1024 MB로
변경되었고 budget은 `min(percent x memory, max(cap, queues x per-queue minimum))`으로
계산된다(사양: `doc/site/docs/spec/core/01-context.ko.md`); 따라서 이전 auto-HWM 기본값으로
측정한 cycle과 비교할 때는 서로 다른 effective HWM을 반드시 반영한다.

Core release version/tag, package provenance, runtime, host boot, CPU governor, client 수, toolchain 또는
성능 관련 환경 변수가 바뀌면 이전 C 결과와 새 binding 결과를 짝지어 판정하지 않는다.
binding before와 after 사이에는 검토 중인 변경만 있어야 하며 변경 파일을 manifest에
기록한다. 그 밖의 조건이 바뀌면 같은 manifest 조건으로 C를 다시 제한 측정한다.

## 7. 실행 절차

공식 entrypoint만 사용한다.

- C single: `bindings/c/perf/run_benchmarks.sh`
- C multi: `bindings/c/perf/run_benchmarks_multi.sh`
- binding single: `bindings/<lang>/perf/run_benchmarks.sh`
- binding multi: `bindings/<lang>/perf/run_benchmarks_multi.sh`

### 7.0 측정 단위와 순차 실행

전체 matrix를 먼저 실행해 기준값을 만드는 방식은 사용하지 않는다. 한 번에 하나의
`binding + suite + pattern + transport` 조합만 비교 대상으로 선택한다. 먼저
`bindings/c/perf`에서 같은 pattern과 transport를 같은 message size, duration, runs,
client 수, I/O thread 수, 해당 작업에서 고정한 Core runtime으로 측정하고, C report가
`status: complete`이면 같은 session tag와 조건으로 해당 binding runner를 바로 측정한다.

선택한 조합의 C와 binding 결과를 비교해 병목과 개선 대상을 정한 뒤, 구현 변경 후에도
같은 조합을 C와 binding 순서로 다시 paired 측정한다. 다른 pattern이나 transport의
결과를 미리 측정하거나, 전체 matrix 결과를 현재 조합의 C 기준으로 재사용하지 않는다.
다른 조합을 확인할 필요가 생기면 기존 측정을 확장하지 않고 새 paired 대상으로 별도로
선택해 같은 절차를 반복한다.

perf **측정**은 항상 직렬화한다. C runner, binding runner, 후보 after 측정 중 어느 것도
동시에 실행하지 않으며, 한 runner process의 report가 종료되고 `status: complete`인지
확인한 뒤 다음 하나의 측정을 시작한다. 백그라운드에서 다른 perf process를 함께 실행해
host CPU·memory·I/O 부하를 섞지 않는다.

직렬화가 걸리는 범위는 **수치를 판정에 쓰는 실행**이다. 구분은 다음과 같다.

| 실행 | 병렬 | 근거 |
|------|------|------|
| 빌드, 컴파일, 단위·contract 테스트, 정적 검사 | 가능 | 수치를 만들지 않는다 |
| **status-only smoke** — 실행 경로와 종료 상태(`status: complete`)만 확인 | 가능 | 판정에 수치를 쓰지 않는다 |
| paired 기준 측정, before/after, 회귀 gate 셀 | **직렬** | 판정 입력이다 |
| **처리량의 자릿수를 판단 근거로 쓰는 smoke** | **직렬** | 아래 참조 |

마지막 항목이 흔한 함정이다. smoke를 "in-flight 1인가 파이프라이닝인가"처럼 크기를 읽는
용도로 쓰는 순간, 그 값은 판정 입력이 된다. 병렬로 돌면 두 프로세스가 서로를 눌러 느린
원인이 코드인지 경합인지 구분할 수 없다. 크기를 읽을 때는 직렬로 다시 잰다.

측정 직전에 `scripts/perf/wait-for-idle-perf.sh`로 다른 perf process가 없는지 확인한다.
있으면 끝날 때까지 기다렸다 시작한다.

### 7.0.1 `PERF_SINGLE_TEST_POLICY` parity gate

비교 기준은 binding runner의 현재 구현이 아니라
`doc/perf/PERF_SINGLE_TEST_POLICY.md`와 그 문서를 반영한 `bindings/c/perf`
canonical reference runner다. 선택한 하나의 비교 대상은 다음 의미가 C와 binding에서
동일해야 유효한 paired 결과로 인정한다.

- `ready -> active(duration)` 순서와 active payload header의 수집 범위
- active 송신의 blocking 의미, transient 오류 후 새 timestamp·1ms retry, stop token의
  wire-level 종료와 bounded retry
- receiver의 `POLLIN` readiness 대기(`-1` timeout)와 `DONTWAIT` drain
- throughput, 평균 latency, p95/p99의 산출 방식과 runs 중앙값 집계
- latency sample cap 기본값 1,000,000, `0`일 때 percentile sample 미보관, 전체 count·sum
  집계, C reference와 동일한 bounded reservoir 교체 알고리즘과 percentile 보간
- 고정한 Core release runtime, auto-HWM message unit, I/O thread 수, client 수와 timeout

위 항목 중 하나라도 C와 binding에서 다르면 수치는 비교 자료로만 남기고 기준값이나
통과 판정에 사용하지 않는다. 정책과 reference runner를 수정한 경우에는 같은
`binding + suite + pattern + transport` 대상의 C와 binding을 다시 순서대로 측정한다.
poller API의 내부 primitive가 다르더라도 readiness, drain, 종료와 metric 의미가 같으면
비교할 수 있다. 반대로 retry 대기나 stop-token 종료 조건이 다르면 수치를 공식 비교에
사용하지 않는다. Single latency sampler가 모든 sample을 무제한으로 보관하거나, sample
cap의 기본값·`0` 처리·percentile 보정이 C reference와 다르면 해당 report도 공식 비교에
사용하지 않는다.

### 7.1 Pattern별 smoke와 제한 사전 점검

전체 pattern이나 전체 matrix를 한 번에 실행해 기준값을 만들지 않는다. 현재 언어에서
진행할 pattern과 transport 하나를 선택한 뒤 C와 binding의 같은 조합만 smoke한다. 이렇게
하면 서로 다른 조합을 측정하는 동안 생기는 host 부하와 시간 차이가 현재 비교값에 섞이지
않는다.

```bash
PERF_FAIL_FAST=1 <c-runner> \
  --pattern <pattern> \
  --msg-sizes 64 \
  --duration 1 \
  --runs 1

PERF_FAIL_FAST=1 <binding-runner> \
  --pattern <pattern> \
  --msg-sizes 64 \
  --duration 1 \
  --runs 1
```

특정 transport나 message size의 병목을 확인할 때도 같은 pattern 안에서만 범위를 제한한다.

```bash
PERF_FAIL_FAST=1 <runner> \
  --pattern <pattern> \
  --transports <transport> \
  --msg-sizes <sizes> \
  --duration 1 \
  --runs 1
```

C와 binding의 pattern별 smoke가 모두 `status: complete`여야 본 측정을 시작한다. console
출력만으로 통과로 판정하지 않는다.

### 7.2 반복 횟수와 측정값 기록

| 단계 | 기본 조건 | 용도 |
|------|-----------|------|
| smoke | 1초, 1회 | 실행 경로와 종료 상태 확인 |
| 탐색 | 기본 duration, 1회 | 병목 후보 선별 |
| 후보 판정 | 기본 duration, 3회 | before/after와 C 대비 비율 판정 |
| 최종·경계 판정 | 기본 duration, 5회, CPU pin 없음 | 필요할 때 반복값을 추가 기록하는 측정 근거 |

반복 횟수는 perf 정책의 실행 조건을 따른다. `runs=1`이면 해당 측정값을 사용하고,
`runs>1`이면 metric별 median을 대표값으로 사용한다. 원시 반복값은 측정 기록에 남기며
판정 입력은 throughput ratio와 평균 latency ratio다. 노트북 부하와 측정 오차가 있더라도
측정값이 생성된 셀은 즉시 기준과 비교하고 다음 셀로 진행한다.
유리한 실행 결과만 선택하지 않으며, CPU pin·timeout·sleep 증가로 수치를 조정하지 않는다.

### 7.3 Paired C 규칙

언어별 통과 판정과 before/after 채택은 현재 진행 중인 pattern에 한정해 가까운 시점에
같은 manifest로 실행한 C 결과를 사용한다. 다른 pattern을 위해 먼저 측정한 C 결과나 이전
라운드의 C full report를 현재 pattern의 판정 기준으로 재사용하지 않는다.

- C와 binding에 같은 session tag를 사용한다.
- 같은 pattern, transport, size, duration, runs, client 수, I/O thread 수를 사용한다.
- C pattern 측정이 끝나면 다른 pattern을 실행하지 않고 바로 같은 binding pattern을 측정한다.
- binding before와 after는 같은 Core release runtime과 host session을 사용하고,
  검토 중인 binding 변경만 다르게 유지한다.
- Core release version/tag, package provenance, runtime, host boot 또는 성능 환경이 달라지면
  C를 다시 측정한다.
- 개선 작업이 길어졌거나 host 부하가 달라졌으면 후보 최종 판정 직전에 같은 C pattern을 다시
  측정한다.
- 목표 기준 ±5%p 셀은 필요하면 추가 반복값을 참고로 기록하지만, 단일 측정값을 무효화하지
  않는다.
- paired report 중 하나라도 `status: complete`가 아니면 표를 갱신하지 않는다.

### 7.4 작업 순서

1. inventory gate를 통과시키고 정책, runner, 상세 표의 측정 범위를 일치시킨다.
2. GitHub `core/v0.17.0` release asset과 package provenance를 준비하고 재현
   환경 manifest를 기록한다. Core source를 다시 build하지 않는다.
3. C++, .NET, Java, Node, Go, Rust, Python 순서로 진행한다.
4. 현재 언어에서 진행할 pattern 하나를 선택한다. C 전체 pattern이나 다음 언어를 미리
   측정하지 않는다.
5. 현재 pattern에서 진행할 transport 하나를 선택한다. Single은 tcp, ws, wss, tls,
   inproc, ipc 순서로 진행하고, runner가 지원하지 않는 transport는 건너뛴다.
6. 선택한 transport의 C pattern만 smoke하고, 바로 같은 binding pattern을 같은 조건으로
   smoke한다.
7. 선택한 pattern과 transport의 모든 message size를 C에서 측정한 직후 binding before를
   측정해 최초 paired 결과를 만든다.
8. C 대비 throughput과 평균 latency를 비교하고 현재 transport의 목표 미달 셀을 확인한다.
9. 미달 셀은 profiler, allocation 자료, copy 수, callback/dispatch 및 native 경계
   자료로 비용 위치를 확인한다.
10. aggregate 평균이 미달한 대상은 먼저 자체 hot-path 개선 pass를 수행한다. 후보는
    public interface, ownership, error contract와 측정 의미를 유지해야 하며, 후보 after를
    한 번 측정해 자체 pass의 효과를 기록한다.
11. 자체 pass 뒤에도 대상의 최종 판단을 닫지 않는다. Sol에 read-only review를 요청하고,
    리뷰에서 선택한 계약 보존 후보로 두 번째 개선 pass를 수행해 after를 한 번 측정한다.
    Sol이 안전한 후보를 제시하지 않으면 그 no-go 판단을 두 번째 pass의 결과로 기록한다.
12. 두 개선 pass의 before/after와 aggregate 결과를 비교한다. 추가 반복은 원인 진단이나
    before/after 확인에 꼭 필요할 때만 수행하며, 변동값을 이유로 반복하지 않는다.
13. 기능 테스트와 같은 pattern 안의 대상이 아닌 대표 셀에 대한 회귀 gate를 통과시킨다.
14. aggregate 평균이 이미 목표를 만족한 대상도 성능 hot path와 POSDDD 리팩토링 요소를
    한 번 검토한다. 공개 contract·ownership·error semantics와 측정 의미를 유지하는
    유효한 후보가 있으면 적용하고 before/after를 측정해 채택 여부를 결정한다. 후보가
    없으면 no-go 근거를 결과 log에 기록한다. 이 검토는 기준 통과를 이유로 생략하지 않는다.
15. 현재 transport의 모든 message size report가 complete이고 throughput·latency aggregate
    평균이 목표를 만족하면 transport 완료를 기록한다. 개별 size 미달은 결과에 기록한다.
    성능 개선 또는 POSDDD 리팩토링을 채택했다면 검증된 변경과 측정 근거만 커밋하고
    원격에 푸시한 뒤 다음 transport로 이동한다.
16. aggregate 평균이 미달한 대상은 자체 pass와 Sol pass가 모두 끝난 뒤에도 공개 contract를
    유지한 성능 또는 POSDDD 이득 후보가 없을 때만 `보류`로 기록하고 다음 transport로
    이동한다. POSDDD 이득만으로 채택한 후보는 성능 aggregate 미달을 통과로 바꾸지 않는다.
17. 선택한 pattern의 모든 공식 transport report가 complete이고 각 transport의
    throughput·latency aggregate 평균이 통과 또는 보류로 확정되면 pattern 완료를 기록하고
    관련 문서를 커밋해 원격에 푸시한다.
18. pattern 커밋과 푸시가 끝난 뒤에만 같은 언어의 다음 pattern을 선택한다.
19. 현재 언어의 Single과 Multi 모든 pattern이 완료된 뒤 pattern별 최종 report와 표를
    다시 대조한다. 미측정 또는 유효한 report가 없는 셀이 남아 있으면 다음 언어로
    이동하지 않는다. aggregate 평균 미달이지만 hot path 검토와 후보 A/B, 필요한 Sol
    리뷰를 끝낸 대상은 `보류`로 기록하고 다음 선택 대상에 진행할 수 있다.
20. 현재 언어가 모두 완료된 뒤에만 다음 언어로 이동한다.

한 번에 하나의 언어만 측정한다. C와 binding을 paired 제한 측정할 때도 공식 perf
프로세스는 순차 실행해 서로 CPU와 memory에 영향을 주지 않게 한다.
모든 최종 측정은 `--pin-cpu`를 사용하지 않는다. 한 번에 perf process 하나만 실행한다.

### 7.5 Pattern 완료와 언어 전환 gate

pattern 완료는 수치를 한 번 얻었다는 뜻이 아니다. 다음 조건을 모두 만족해야 완료로
기록한다.

- 해당 pattern의 모든 공식 transport와 message size에서 C와 binding report가
  `status: complete`다.
- 모든 size의 paired report가 있고, throughput ratio 산술평균과 평균 latency ratio의
  산술평균, client 수, auto-HWM 기준을 측정값으로 판정한다. 개별 size의 최소 기준·latency
  상한 미달은 결과에 기록하되 aggregate gate를 별도로 낮추지 않는다.
- 개선 전후 기능 테스트와 같은 pattern의 대표 회귀 셀이 통과한다.
- 최종 판정에 사용한 C와 binding이 가까운 시점의 같은 manifest와 session tag로 측정됐다.
- 상세 표에 C report, binding report, 반복값, 비율과 판정 근거를 기록했다.
- POSDDD 위험 신호를 변경 전후로 다시 확인했고 새 복잡성을 만들지 않았다.

목표에 미달하면 자체 개선 pass와 Sol 리뷰 기반 개선 pass를 각각 한 번씩 수행한다. 각
pass는 before/after 또는 후보 no-go 결과를 남긴다. 두 pass가 끝난 뒤에도 공개 contract를
유지한 효과 있는 후보가 없으면 `보류`로 확정한다. 변동값과 안정성을 이유로 같은 셀을
반복하지 않는다. public contract 변경이 필요하면 우회 구현으로 통과시키지 않고 `보류`로
기록한다.

### 7.6 개선 코드 커밋과 푸시

public/runtime 경계, ownership, callback 수명, allocator, queue 또는 thread model을
바꾸는 구조 변경은 구현·측정 전에 Sol 에이전트에 read-only review를 요청한다. 리뷰에는
변경 범위, 계약과 수명 영향, 예상 비용, A/B 측정 방법을 함께 전달한다. 리뷰 결과와
before/after 측정으로 이득이 분리되지 않거나 cleanup·thread 이동·예외 경로 위험이 남으면
후보를 채택하지 않고 제거한다. C harness parity 수정과 binding source 개선은 별도 후보로
분리해 각각의 효과와 책임을 기록한다.

성능 개선 후보가 pattern 목표와 회귀 gate를 통과해 최종 코드로 채택되면 다음 pattern을
시작하기 전에 커밋하고 원격 저장소에 푸시한다. 커밋에는 현재 개선과 직접 관련된 binding,
테스트, runner, 계획 문서와 측정 로그만 포함한다. 작업 트리의 다른 변경을 함께 넣지 않는다.

커밋 전에는 변경 파일 목록과 staged diff를 확인하고 `git diff --cached --check`를 통과시킨다.
커밋 메시지에는 언어와 pattern, 제거한 병목을 드러낸다. 푸시한 commit id와 paired report
경로를 라운드 기록에 남긴다. 다음 상태는 커밋 대상으로 인정하지 않는다.

- C 또는 binding report가 partial인 후보
- 목표나 latency, 회귀 gate를 통과하지 못한 후보
- perf 전용 우회나 public contract 위반이 남은 후보
- 기능 테스트를 통과하지 못한 후보

C++ binding 경로에서는 large-message buffer pool을 사용하지 않는 것으로 확정한다. C++의
pool 재도입이나 pool A/B는 후보로 취급하지 않는다. 반면 .NET과 같이 VM 또는 managed
runtime 위에서 동작하는 binding에 이미 존재하는 `Message`·byte storage pool은 기존 내부
구현으로 유지할 수 있다. managed binding에서 pool을 새로 도입하거나 정책을 바꿀 때도
public ownership과 재사용 경계를 바꾸지 않고, 별도 후보로 before/after 측정을 남긴다.
후보를 기각했으면 코드를 최종 변경에서 제거하고 측정 결과와 기각 이유만 로그에 남긴다.

### 7.7 성능 개선의 POSDDD gate

성능 병목을 찾으면 구현 전에 현재 코드의 위험 신호를 먼저 적는다. 최소한 얕은 모듈,
정보 누출, 패스스루 메서드, 실행 순서에 따른 책임 분리, 특수 코드와 범용 코드의 혼합,
반복 지식을 확인한다. 각 위험 신호가 어떤 책임 경계에서 생겼는지 설명하고 서로 다른
개선 방향을 두 가지 이상 비교한다.

성능 개선은 호출자가 알아야 할 설정과 순서를 늘리지 않고 binding 내부에서 비용을 흡수해야
한다. hot path의 allocation, copy, 검증, dispatch를 줄이더라도 public API에 내부 자료구조,
transport 세부 정보, perf 전용 option을 노출하지 않는다. 새 helper나 class가 단순 전달만
한다면 추가하지 않고 기존 모듈의 책임을 깊게 만든다.

후보 측정 뒤에는 다음 순서로 판정한다.

1. 측정 가능한 성능 향상이 있으면 성능·기능 회귀와 복잡성 증가 여부를 함께 확인한다.
2. 성능 향상이 없더라도 기존 위험 신호를 없애고 정보 은닉이나 책임 경계를 분명하게
   개선하며, 처리량·평균 latency·기능 회귀가 없으면 POSDDD 개선으로 채택할 수 있다.
3. 성능과 POSDDD 어느 쪽에서도 분명한 이득이 없거나 성능 회귀가 생기면 복잡성을 남기지
   않고 되돌린다. POSDDD 개선만으로 throughput 미달 셀을 통과로 바꾸지는 않는다.
4. 성능 목표를 만족해도 public interface와 호출자 부담이 커졌으면 채택하지 않는다.
5. 채택 가능한 후보 중 정보 은닉과 책임 경계가 더 분명한 설계를 선택한다.
6. 변경 뒤 같은 위험 신호 목록을 다시 확인해 해소 여부와 새 위험 신호를 기록한다.
7. source comment는 코드가 반복하는 설명이 아니라 유지해야 할 계약과 설계 이유만 남긴다.

## 8. 판정과 기록 방법

상태 값은 다음과 같이 사용한다.

- `미측정`: 같은 조건의 core 0.17.0 C 결과와 binding 결과를 아직 비교하지 않았다.
- `통과(비율%)`: 모든 size의 paired report가 complete이고, throughput ratio 산술평균과
  평균 latency ratio의 산술평균, 회귀, Effective Options, auto-HWM, client 수 조건을 만족한다.
  개별 size의 최소 기준·latency 상한 미달은 outlier로 함께 기록할 수 있다.
- `미달(비율%)`: 최종 판정 전의 임시 상태다. 유효한 paired 결과가 있지만 throughput 또는
  latency의 aggregate 평균 목표에 도달하지 않았고 hot path 검토 또는 후보 비교가 남아 있다.
- `보류`: paired 측정, 자체 개선 pass, Sol 리뷰 기반 두 번째 개선 pass를 완료했지만
  public contract를 유지한 추가 개선 요소가 없어 현재 aggregate 목표를 달성하지 못한 채
  다음 대상으로 이동한다. 변동 폭이나 안정성을 이유로 `보류`하지 않는다.
- 상세 표의 최종 상태는 `통과(비율%)`, `보류(비율%)`, `미측정`을 사용한다. C와 binding의
  paired report가 있고 ratio와 latency가 기록된 pattern·transport이면 size ratio와 평균
  latency ratio의 aggregate 평균으로 상태를 판정한다. paired report가 없는 셀만
  `미측정`으로 남긴다.
- 측정 여부는 두 report 경로가 있고 두 report가 모두 `status: complete`이며 ratio가
  기록되어 있는지로 확인한다. 이 조건이면 `측정 완료`이고, 셀 값이 `미측정`이면
  아직 측정하지 않은 것이다. `측정값으로 판정` 같은 문구는 상태 값으로 사용하지 않는다.
- `해당 없음`: 공식 C runner와 binding 정책 모두 측정하지 않는 조합이다.

원시 반복값이나 하향 drift는 필요한 경우 측정 기록으로만 남긴다. 이것만으로 `미측정`
또는 별도 판정 상태를 만들지 않는다. 측정값이 있으면 throughput ratio와 latency ratio의
aggregate 평균으로 판정하고, aggregate 목표가 미달이면 hot path 검토와 후보 A/B, 필요한
Sol 리뷰 후 즉시 `보류` 여부를 결정한다. public contract 변경이 필요한 후보는 채택하지
않고 현재 interface를 유지한다.

과거 문서나 로그의 분류는 이력으로 보존하되, C와 binding report가 모두 `status: complete`이고
ratio가 기록되어 있으면 size별 ratio의 aggregate 평균과 평균 latency ratio의 aggregate
평균을 사용해 `통과`, `미달` 또는 `보류`로 평가한다. 원시 반복값은 함께 기록하지만 변동 폭을 이유로
판정을 미루지 않는다. `미측정`은 paired report 자체가 없을 때만 사용한다.

timeout, no result, runtime mismatch, message size 불일치, client 수 불일치는 성능 판정이
아니다. 원인을 수정해 수치가 생성될 때까지 `미측정`으로 유지한다.

계획서의 결과 표에는 다음 측정 기록과 결과만 남긴다. 나머지 과정은 `log/`에 남긴다.

- paired session tag
- C report와 binding report 경로
- 두 report의 runtime 경로와 실제 core 버전
- throughput 비율과 개별 반복값
- 평균 latency 비율과 개별 반복값. p95와 p99는 진단 자료로만 기록한다.
- throughput과 평균 latency의 원시 반복값
- Effective Options 일치 여부
- auto-HWM의 `MsgUnit(B)` 일치 여부
- 실제 client 수, STREAM client 수, memory guard cap 발생 여부
- 반복 측정값과 최종 판정
- 필요한 경우 판정에 사용하지 않은 진단값과 제외 이유

## 9. 언어별 성능 확인 표

> **Single suite의 pattern (D-BP6)**: Single은 `PAIR`, `PUBSUB`, `DEALER_DEALER`,
> `DEALER_ROUTER`, `ROUTER_ROUTER`, `DEALER_ROUTER_REQREP`, `ROUTER_ROUTER_REQREP`
> 7개 pattern을 측정한다. request/reply를 Single에서 제외했던 D-BP3은 철회됐다 —
> 동기 실행 모델은 "한 건 보내고 응답 대기"가 아니라 "전용 OS thread가 진행을 직접
> 구동"이라는 뜻이고, admission과 reply는 Core 공개 API에서 이미 별개 사건이므로
> (`core/include/zlink/socket/api.h`의 `zlink_request_part`) Single에서도
> backpressure 경계까지 연속 제출할 수 있다. 실행 형태는
> `PERF_SINGLE_TEST_POLICY.md` §1.1이 소유한다.
>
> **Single 상세 표의 상태 값 (D-BP2)**: `x.1 Single suite` 표의 셀은 **기록**이며 성능
> 판정이 아니다. 기존에 기록된 `통과`·`미달` 값과 비율은 그대로 보존한다(측정은 실제로
> 이루어졌고 값도 유효하다). 다만 `PERF_POLICY.md:144-149`에 따라 이 값들을 cross-binding
> 성능 gate로 사용하지 않는다. 성능 판정은 `x.2 Multi suite` 표에서만 한다.

모든 언어는 같은 열과 같은 상태 규칙을 사용한다. 상세 표의 상태가 진행 상태 요약보다
우선한다. 상세 표에 `미측정` 또는 `미달`이 하나라도 남아 있으면
해당 언어는 완료가 아니다.

> **러너 정합 이전 값은 현재 판정이 아니다 (D-BP19).** 2026-09-05/06에 기록된 Multi 판정은
> 모두 러너가 바뀌기 전 값이다. 그 뒤 C와 7개 binding의 러너가 정책 기준으로 정합됐고
> (`87153dd4f3`, `d634417a37`, `522df6d57e`, `074d2a5964`, `7549a128b1`, `d51c16b285`,
> `31c5e4f7f0`) 같은 셀의 값이 크게 달라졌다 — C++ `tcp` `MULTI_DEALER_ROUTER_REQREP`
> 57.4%→72.87%, `MULTI_DEALER_DEALER` 90.8%→95.42%. 따라서 **정합 이후 tag(C++ `p3pin`·
> `p5cmodel`)로 다시 잰 셀만 판정으로 쓰고, 나머지 옛 값은 `미측정`과 같게 취급해 §7.4대로
> 다시 잰다.** 옛 수치는 지우지 않고 참고값으로 남긴다 — §10.3대로 유지하는 것은 수치가
> 아니라 개선 pass 코드·프로파일·no-go 목록 같은 작업 자산이다.

### 9.1 C++

- perf 경로: `bindings/cpp/perf`
- Single 상태: `미달`(REQREP만) — before 07:16 KST(7 pattern × 6 transport, [log](log/2026-09-05-cpp-single-before.ko.md)) → 수신 경로 자체 pass 1(library no-go, C++ 러너 `PERF_PART_COUNT` getenv/메시지 버그 수정 `9cb8a3a11b`, [log](log/2026-09-05-cpp-single-recv-pass1.ko.md)) → one-way 5 pattern 재짝지음 08:20 KST: PAIR 86.3~95.9%(tls·ipc `통과`), PUBSUB 91.1~113.2%(inproc·wss 외 `통과`), DD 63.1~96.3%(inproc 외 `통과`, 완화 90%), DR 90.6~97.3% 전부 `통과`, RR 73.8~98.2%(inproc 외 `통과`). one-way 평균 latency는 C 대비 수 배~수백 배이나 두 러너 정의가 같고 C++ 수신이 송신보다 느려 큐가 HWM까지 차는 큐 깊이 값이라 판정에서 제외(처리량으로 판정, D-B91). REQREP 40.6~46.4% `미달`(REQUEST async 경로, pass 예정).
- Multi 상태: **0.17.2 기준 24 cell 기록 완료, 열린 조사 없음** — `통과` 10(tcp DD 97.56·PUBSUB 95.32·DR SS 91.86·RR SS 91.67; tls DD 95.69·PUBSUB 94.61·DR SS 94.95; ws DD 95.24·PUBSUB 95.44; wss PUBSUB 95.15). `보류` 14 = REQREP 4(tcp·tls, D-BP26 구조적 근거: 요청당 고정 비용이 지배적 항목 없이 퍼져 있음) + **Core/transport 귀속 10**(ws·wss REQREP 4는 왕복 처리량 붕괴, tls RR·ws DR/RR·wss DR/RR SENDSEND 5는 간헐 latency 폭증 — 둘 다 C에서 재현, D-BP28·D-BP29; STREAM은 4 transport를 1로 셈). `차단` 1(wss DD — C 러너 간헐 실패, 깨끗한 재현 1승 1패, perf 조용할 때 재시도). **C++ 쪽에서 더 할 일은 없다** — 남은 보류는 Core 수정(보고서 2건: STREAM 정체 D-BP23, ws/wss 왕복 D-BP28·D-BP29) 뒤 재측정으로만 풀린다. C++ 러너 정합 수정 3건(relay 직렬화, client echo drain, C `ctx_term`)과 REQREP 비용 지도(D-BP26)는 다른 언어에 그대로 쓰인다.
- 다음 작업: `tcp` REQREP pass 2 판정 → `tcp` SENDSEND 2종 → `MULTI_STREAM` smoke 조사 후 측정 → `tls`·`ws`·`wss` 7 pattern 재측정. `131072`는 §3.2대로 측정 범위 밖이다.

#### 9.1.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 미달(80.0%) | 미달(81.2%) | 미달(97.6%) | 미달(92.6%) | 미달(94.4%) | 미달(94.2%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 90.0%(목표 95%), latency 13.40x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 1052.5/1315.5, 944.5/1163.6, 797.1/816.9, 59.1/63.9, 33.8/35.8, 17.1/18.1 Kmsg/s; latency ms C++·C 0.158/0.028, 1.019/0.029, 0.895/0.030, 0.223/0.049, 0.198/0.064, 0.202/0.094; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_073750_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_074210_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `tcp` | `PUBSUB` | 통과(95.9%) | 통과(92.5%) | 통과(105.7%) | 통과(99.5%) | 통과(137.3%) | 통과(146.7%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 112.9%(목표 95%), latency 4.60x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 840.7/876.8, 777.9/840.8, 656.9/621.6, 12.7/12.8, 8.2/6.0, 3.7/2.5 Kmsg/s; latency ms C++·C 0.081/0.051, 0.232/0.054, 0.656/0.051, 0.251/0.069, 0.252/0.082, 0.253/0.115; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_074600_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_075055_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `tcp` | `DEALER_DEALER` | 통과(80.8%) | 통과(85.7%) | 통과(105.8%) | 통과(92.3%) | 통과(93.5%) | 통과(97.7%) | after(완화 목표 90% 적용; runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 92.6%(목표 95%), latency 72.95x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 894.1/1106.9, 880.4/1027.4, 833.6/787.8, 37.7/40.9, 24.1/25.8, 14.9/15.3 Kmsg/s; latency ms C++·C 17.388/0.046, 1.497/0.049, 0.976/0.050, 0.359/0.069, 0.287/0.085, 0.251/0.121; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_075521_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_075941_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `tcp` | `DEALER_ROUTER` | 통과(88.3%) | 통과(91.0%) | 통과(98.9%) | 통과(97.6%) | 통과(95.9%) | 통과(99.2%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 95.2%(목표 85%), latency 6.99x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 909.1/1029.2, 861.6/946.7, 732.0/740.1, 37.6/38.6, 24.4/25.4, 15.1/15.2 Kmsg/s; latency ms C++·C 0.256/0.052, 0.391/0.052, 1.034/0.055, 0.359/0.070, 0.284/0.084, 0.246/0.121; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_080331_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_080750_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `tcp` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `tcp` | `ROUTER_ROUTER` | 통과(92.4%) | 통과(93.3%) | 통과(90.9%) | 통과(99.1%) | 통과(95.5%) | 통과(98.5%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 94.9%(목표 85%), latency 54.25x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 893.5/967.4, 823.5/882.8, 665.0/731.5, 38.5/38.9, 23.3/24.4, 14.5/14.8 Kmsg/s; latency ms C++·C 0.097/0.051, 2.961/0.050, 12.984/0.051, 0.351/0.071, 0.295/0.087, 0.257/0.118; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_081141_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_081600_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `tcp` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ws` | `PAIR` | 미달(83.9%) | 미달(94.3%) | 미달(91.6%) | 미달(97.2%) | 미달(98.1%) | 미달(99.4%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 94.1%(목표 95%), latency 984.25x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 968.2/1153.3, 900.2/954.8, 618.7/675.1, 32.2/33.2, 19.7/20.1, 10.4/10.5 Kmsg/s; latency ms C++·C 92.888/0.025, 41.032/0.024, 12.372/0.033, 4.837/0.072, 0.333/0.097, 0.342/0.142; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_073750_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_074210_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `ws` | `PUBSUB` | 통과(90.0%) | 통과(94.0%) | 통과(94.4%) | 통과(98.7%) | 통과(98.1%) | 통과(131.0%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 101.0%(목표 95%), latency 431.50x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 789.4/877.1, 755.1/803.3, 585.5/620.4, 12.7/12.9, 5.9/6.0, 3.3/2.5 Kmsg/s; latency ms C++·C 78.642/0.049, 37.566/0.055, 16.148/0.053, 0.394/0.081, 0.295/0.099, 0.290/0.136; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_074600_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_075055_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `ws` | `DEALER_DEALER` | 통과(91.3%) | 통과(89.0%) | 통과(102.2%) | 통과(98.1%) | 통과(96.1%) | 통과(98.2%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 95.8%(목표 95%), latency 554.06x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 903.9/989.6, 835.3/938.1, 586.9/574.1, 23.7/24.1, 15.7/16.3, 9.5/9.7 Kmsg/s; latency ms C++·C 116.574/0.055, 43.906/0.049, 12.663/0.053, 5.183/0.090, 0.430/0.113, 0.388/0.170; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_075521_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_075941_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `ws` | `DEALER_ROUTER` | 통과(91.6%) | 통과(92.1%) | 통과(94.1%) | 통과(98.7%) | 통과(98.0%) | 통과(99.3%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 95.6%(목표 85%), latency 529.42x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 903.6/986.6, 820.2/890.6, 543.7/578.1, 24.1/24.4, 15.9/16.2, 9.5/9.6 Kmsg/s; latency ms C++·C 99.650/0.052, 44.597/0.051, 18.166/0.055, 5.144/0.088, 0.424/0.113, 0.393/0.164; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_080331_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_080750_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `ws` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ws` | `ROUTER_ROUTER` | 통과(93.3%) | 통과(95.3%) | 통과(93.7%) | 통과(98.3%) | 통과(95.9%) | 통과(98.6%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 95.9%(목표 85%), latency 536.27x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 867.1/929.0, 805.9/845.4, 529.5/565.2, 23.6/24.0, 15.5/16.1, 9.5/9.6 Kmsg/s; latency ms C++·C 102.844/0.050, 40.275/0.051, 16.083/0.054, 5.205/0.086, 0.436/0.111, 0.388/0.178; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_081141_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_081600_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `ws` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `wss` | `PAIR` | 미달(84.7%) | 미달(91.5%) | 미달(95.3%) | 미달(95.3%) | 미달(96.9%) | 미달(99.7%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 93.9%(목표 95%), latency 942.96x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 1056.6/1247.5, 781.6/854.2, 402.9/422.8, 11.1/11.7, 5.9/6.1, 3.2/3.2 Kmsg/s; latency ms C++·C 64.814/0.022, 41.127/0.025, 24.164/0.025, 12.710/0.145, 10.874/0.234, 10.535/0.416; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_073750_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_074210_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `wss` | `PUBSUB` | 미달(89.6%) | 미달(97.8%) | 미달(101.6%) | 미달(103.0%) | 미달(95.2%) | 미달(81.4%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 94.8%(목표 95%), latency 482.39x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 816.7/911.5, 682.8/698.1, 304.5/299.6, 10.6/10.3, 5.7/6.0, 2.1/2.6 Kmsg/s; latency ms C++·C 76.895/0.055, 50.034/0.059, 31.231/0.060, 10.544/0.172, 12.163/0.231, 0.839/0.400; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_074600_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_075055_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `wss` | `DEALER_DEALER` | 통과(85.7%) | 통과(88.6%) | 통과(100.0%) | 통과(97.1%) | 통과(98.4%) | 통과(96.6%) | after(완화 목표 90% 적용; runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 94.4%(목표 95%), latency 507.93x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 890.0/1038.1, 704.9/795.8, 312.9/313.0, 9.7/10.0, 5.6/5.7, 3.1/3.2 Kmsg/s; latency ms C++·C 96.495/0.055, 38.176/0.058, 31.116/0.062, 13.493/0.173, 10.609/0.256, 9.096/0.434; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_075521_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_075941_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `wss` | `DEALER_ROUTER` | 통과(85.8%) | 통과(95.7%) | 통과(97.4%) | 통과(99.7%) | 통과(99.4%) | 통과(98.7%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 96.1%(목표 85%), latency 533.81x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 928.3/1082.1, 733.2/765.9, 305.5/313.8, 9.8/9.8, 5.6/5.7, 3.1/3.1 Kmsg/s; latency ms C++·C 102.669/0.058, 46.312/0.058, 31.865/0.065, 14.243/0.171, 9.861/0.262, 8.863/0.445; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_080331_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_080750_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `wss` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `wss` | `ROUTER_ROUTER` | 통과(95.6%) | 통과(98.1%) | 통과(98.5%) | 통과(98.8%) | 통과(98.2%) | 통과(100.2%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 98.2%(목표 85%), latency 457.66x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 928.3/971.0, 706.9/720.6, 298.2/302.6, 9.7/9.8, 5.5/5.6, 3.1/3.1 Kmsg/s; latency ms C++·C 73.836/0.061, 50.858/0.059, 31.229/0.060, 14.405/0.170, 12.197/0.255, 11.220/0.454; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_081141_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_081600_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `wss` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `tls` | `PAIR` | 통과(85.6%) | 통과(92.0%) | 통과(100.2%) | 통과(98.5%) | 통과(97.9%) | 통과(98.2%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 95.4%(목표 95%), latency 26.51x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 1100.5/1284.9, 898.3/976.2, 489.8/489.1, 14.5/14.8, 7.5/7.7, 3.9/3.9 Kmsg/s; latency ms C++·C 0.403/0.020, 2.425/0.031, 1.578/0.035, 0.844/0.117, 0.844/0.184, 0.877/0.315; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_073750_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_074210_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `tls` | `PUBSUB` | 통과(91.1%) | 통과(101.9%) | 통과(104.9%) | 통과(100.1%) | 통과(92.9%) | 통과(85.5%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 96.1%(목표 95%), latency 15.20x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 883.9/970.2, 742.6/728.5, 329.5/314.1, 12.1/12.1, 5.6/6.1, 2.2/2.5 Kmsg/s; latency ms C++·C 0.142/0.053, 2.227/0.054, 2.052/0.056, 0.720/0.149, 0.667/0.193, 0.700/0.347; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_074600_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_075055_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `tls` | `DEALER_DEALER` | 통과(88.6%) | 통과(96.0%) | 통과(97.9%) | 통과(97.8%) | 통과(96.5%) | 통과(100.9%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 96.3%(목표 95%), latency 290.99x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 939.3/1060.7, 845.5/881.0, 371.6/379.7, 12.0/12.3, 7.0/7.3, 4.0/4.0 Kmsg/s; latency ms C++·C 87.769/0.053, 2.689/0.055, 2.051/0.058, 1.050/0.140, 0.935/0.197, 0.896/0.327; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_075521_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_075941_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `tls` | `DEALER_ROUTER` | 통과(84.7%) | 통과(96.5%) | 통과(103.2%) | 통과(101.3%) | 통과(98.0%) | 통과(100.0%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 97.3%(목표 85%), latency 296.04x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 930.3/1098.4, 851.7/883.0, 365.2/354.0, 12.3/12.1, 7.1/7.3, 4.0/4.0 Kmsg/s; latency ms C++·C 93.437/0.056, 2.711/0.056, 2.085/0.056, 1.027/0.140, 0.909/0.209, 0.897/0.350; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_080331_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_080750_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `tls` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `tls` | `ROUTER_ROUTER` | 통과(91.9%) | 통과(93.7%) | 통과(97.2%) | 통과(99.5%) | 통과(100.2%) | 통과(100.0%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 97.1%(목표 85%), latency 223.37x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 920.1/1000.9, 762.9/814.3, 442.2/454.9, 12.3/12.3, 7.2/7.1, 3.9/3.9 Kmsg/s; latency ms C++·C 0.359/0.057, 48.479/0.055, 18.576/0.056, 10.268/0.146, 8.570/0.218, 7.899/0.368; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_081141_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_081600_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `tls` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `inproc` | `PAIR` | 미달(87.2%) | 미달(84.9%) | 미달(85.2%) | 미달(83.4%) | 미달(82.5%) | 미달(94.6%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 86.3%(목표 95%), latency 466.30x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 1269.5/1456.4, 1043.7/1229.4, 1056.9/1241.1, 290.1/347.9, 191.4/232.0, 128.5/135.8 Kmsg/s; latency ms C++·C 3.330/0.002, 2.077/0.002, 0.478/0.002, 0.005/0.004, 0.007/0.005, 0.010/0.008; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_073750_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_074210_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `inproc` | `PUBSUB` | 미달(95.0%) | 미달(96.8%) | 미달(96.1%) | 미달(47.3%) | 미달(100.1%) | 미달(111.6%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 91.1%(목표 95%), latency 2.17x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 856.1/900.7, 722.9/747.0, 707.4/736.3, 78.9/166.9, 60.5/60.5, 18.8/16.8 Kmsg/s; latency ms C++·C 0.020/0.008, 0.027/0.007, 0.030/0.007, 0.024/0.022, 0.012/0.024, 0.022/0.027; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_074600_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_075055_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `inproc` | `DEALER_DEALER` | 미달(93.3%) | 미달(103.3%) | 미달(95.0%) | 미달(30.6%) | 미달(27.0%) | 미달(29.4%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 63.1%(목표 95%), latency 154.78x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 1003.5/1075.6, 958.4/927.5, 860.4/905.6, 110.7/362.2, 57.9/213.9, 23.0/78.2 Kmsg/s; latency ms C++·C 4.411/0.008, 2.262/0.008, 0.808/0.007, 0.024/0.024, 0.032/0.028, 0.062/0.034; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_075521_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_075941_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `inproc` | `DEALER_ROUTER` | 통과(89.0%) | 통과(88.0%) | 통과(92.2%) | 통과(83.8%) | 통과(92.5%) | 통과(98.1%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 90.6%(목표 85%), latency 160.00x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 1051.8/1182.1, 908.9/1032.8, 879.2/953.9, 321.6/383.8, 203.0/219.6, 76.6/78.0 Kmsg/s; latency ms C++·C 4.154/0.008, 2.390/0.008, 0.800/0.007, 0.029/0.024, 0.014/0.028, 0.025/0.034; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_080331_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_080750_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `inproc` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `inproc` | `ROUTER_ROUTER` | 미달(89.5%) | 미달(90.8%) | 미달(92.4%) | 미달(49.4%) | 미달(42.2%) | 미달(78.4%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 73.8%(목표 85%), latency 146.85x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 1053.3/1177.2, 919.7/1012.6, 896.7/970.9, 174.6/353.7, 82.2/195.0, 62.4/79.5 Kmsg/s; latency ms C++·C 0.285/0.007, 4.884/0.008, 1.683/0.008, 0.021/0.025, 0.022/0.029, 0.026/0.034; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_081141_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_081600_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `inproc` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ipc` | `PAIR` | 통과(86.4%) | 통과(90.7%) | 통과(96.4%) | 통과(99.5%) | 통과(101.6%) | 통과(100.7%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 95.9%(목표 95%), latency 9.82x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 1036.4/1199.0, 948.5/1045.3, 846.4/878.3, 64.4/64.7, 35.4/34.8, 18.2/18.1 Kmsg/s; latency ms C++·C 0.168/0.028, 0.339/0.027, 0.869/0.028, 0.206/0.048, 0.192/0.061, 0.185/0.089; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_073750_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_074210_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `ipc` | `PUBSUB` | 통과(92.5%) | 통과(92.3%) | 통과(105.0%) | 통과(98.8%) | 통과(143.3%) | 통과(147.1%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 113.2%(목표 95%), latency 4.24x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 857.2/926.6, 783.8/849.1, 753.8/717.9, 12.8/12.9, 8.6/6.0, 3.7/2.5 Kmsg/s; latency ms C++·C 0.057/0.046, 0.151/0.048, 0.540/0.046, 0.234/0.063, 0.248/0.076, 0.250/0.109; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_074600_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_075055_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `ipc` | `DEALER_DEALER` | 통과(89.4%) | 통과(87.2%) | 통과(99.4%) | 통과(94.8%) | 통과(93.4%) | 통과(99.6%) | after(완화 목표 90% 적용; runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 94.0%(목표 95%), latency 7.95x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 967.2/1082.2, 846.5/970.4, 824.0/829.3, 40.4/42.6, 25.0/26.8, 16.1/16.2 Kmsg/s; latency ms C++·C 0.474/0.047, 0.403/0.046, 0.884/0.048, 0.336/0.069, 0.277/0.079, 0.230/0.115; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_075521_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_075941_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `ipc` | `DEALER_ROUTER` | 통과(85.3%) | 통과(85.6%) | 통과(105.4%) | 통과(96.9%) | 통과(97.3%) | 통과(99.8%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 95.0%(목표 85%), latency 5.92x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 945.6/1108.4, 846.7/989.4, 871.3/826.9, 39.3/40.6, 25.8/26.5, 16.1/16.1 Kmsg/s; latency ms C++·C 0.221/0.048, 0.242/0.047, 0.712/0.046, 0.346/0.070, 0.269/0.080, 0.231/0.117; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_080331_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_080750_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `ipc` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ipc` | `ROUTER_ROUTER` | 통과(91.3%) | 통과(90.0%) | 통과(98.7%) | 통과(97.3%) | 통과(98.4%) | 통과(99.4%) | after(runner PERF_PART_COUNT 수정 `9cb8a3a11b` 뒤 C·C++ 재짝지음; library 변경 없음); aggregate throughput 95.8%(목표 85%), latency 7.04x(one-way latency는 큐 깊이 — 판정 제외, 처리량으로 판정); 처리량 C++·C 897.0/982.7, 796.2/884.4, 791.4/802.0, 39.7/40.8, 25.8/26.2, 15.8/15.9 Kmsg/s; latency ms C++·C 0.123/0.050, 0.245/0.048, 1.155/0.047, 0.341/0.068, 0.267/0.082, 0.236/0.118; `p1cpp-single-fix`; C `perf_c_single_linux_20260905_081141_p1cpp-single-fix.txt`, C++ `perf_cpp_single_linux_20260905_081600_p1cpp-single-fix.txt`; [log](log/2026-09-05-cpp-single-recv-pass1.ko.md) |
| `ipc` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |

#### 9.1.2 Multi suite

> **이 표의 `tls`·`ws`·`wss` 행과 2026-09-05 tag(`p1cpp*`)를 단 모든 행은 러너 정합 이전 값이라
> 현재 판정이 아니다.** 2026-09-05 이후 C와 7개 binding의 러너가 정책 기준으로 바뀌었다 —
> `87153dd4f3`(8개 러너 측정 조건 일치), `d634417a37`(multi 종료·부하 모델), `522df6d57e`·`074d2a5964`
> (single REQREP 복원), `7549a128b1`(part-count `getenv` 제거), `d51c16b285`, `31c5e4f7f0`(상한 제거·C
> turn 구조 복원). 같은 `tcp` `MULTI_DEALER_ROUTER_REQREP`이 57.4%→72.87%로, `MULTI_DEALER_DEALER`가
> 90.8%→95.42%로 달라진 것이 그 차이다. **현재 판정으로 쓸 수 있는 행은 `p5cmodel`·`p3pin` tag를 단
> `tcp` 4개 pattern뿐이며, 나머지는 `미측정`과 같게 취급하고 §7.4대로 다시 잰다.** 아래 행의 옛 수치는
> 참고값으로만 남긴다(§10.3의 "유지하는 것은 수치가 아니라 작업 자산이다"와 같은 성격).

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 통과(84.3%) | 통과(85.4%) | 통과(104.1%) | 통과(103.5%) | 통과(110.6%) | 미측정 | **`통과(97.56%)` 확정 — 기본 목표 95% 충족, 완화 목표 불필요**; latency 0.713x `통과`; 개별 최소 85% 미달 outlier는 64 B(84.31%) 하나. **0.17.1의 94.73%에서 2.83%p 올랐다** — 전 크기가 고르게 개선됐고(64 B 82.12→84.31%, 1024 B 101.75→104.08%, 65536 B 109.13→110.55%) DD는 러너 정합 수정 경로(routed relay·echo client)를 쓰지 않으므로 **0.17.2 Core 자체의 개선**이다. 처리량 C++·C 1,461.2/1,733.1, 1,234.9/1,446.1, 1,241.1/1,192.5, 684.8/661.9, 182.7/165.3 Kmsg/s; §7.2 5-run, 고정 Core **0.17.2** prefix(태그 `core/v0.17.2` `dca377aa5e`, Build ID `c5dec25e…`), `q3tcp`, C·C++ 모두 `status: complete`. 0.17.1 값은 참고값이다(D-B216 재고정). |
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 통과(93.5%) | 통과(94.8%) | 통과(90.2%) | 통과(93.4%) | 통과(87.5%) | 미측정 | **`통과(91.86%, 완화 목표 90%)` 확정**; latency 1.176x `통과`; 개별 최소 85% 미달 없음(최저 65536 B 87.49%). 0.17.1의 92.37%와 0.51%p 차이다. 완화 근거는 D-BP22 — pass 1이 routed send 고유 비용에 격차가 없음을 보였고(native routed send inclusive C 5,278 / C++ 5,278 Ir), 남은 것은 D-BP26이 지도로 확인한 **요청당 고정 비용**이며 지배적 항목 없이 380~830 Ir짜리 10개 항목에 퍼져 있다. 처리량 C++·C 451.9/483.2, 419.4/442.6, 405.6/449.7, 371.3/397.6, 71.0/81.2 Kmsg/s; §7.2 5-run, 고정 Core **0.17.2** prefix(태그 `core/v0.17.2` `dca377aa5e`, Build ID `c5dec25e…`), `q3tcp`, C·C++ 모두 `status: complete`. 0.17.1 값은 참고값이다(D-B216 재고정). |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 보류(72.4%) | 보류(75.5%) | 보류(70.1%) | 보류(73.4%) | 보류(71.7%) | 미측정 | **`보류(72.61%)` 확정**(목표 85%), latency 2.465x(목표 2.0x, 65536 B 8.41x가 끌어올린다); 처리량 C++·C 288.6/398.6, 274.5/363.5, 266.3/379.8, 252.7/344.4, 45.3/63.3 Kops/s; §7.2 5-run, 고정 Core **0.17.2**(태그 `core/v0.17.2` `dca377aa5e`), `q4tcp`, C·C++ 모두 `status: complete`. **0.17.2가 REQREP을 개선하지 않았다** — 같은 재측정에서 `MULTI_DEALER_DEALER`는 94.73→97.56%(+2.83%p)로 올랐는데 REQREP은 0.3~0.6%p 차이로 사실상 같다. 0.17.2의 completion pull 결함 수정이 REQREP을 느리게 만드는 부분에 닿지 않았다는 뜻이며, **격차가 Core가 아니라 C++ binding의 application 계층에 있다는 D-BP26의 비용 지도와 일치한다**. pass 1·2 완료 후 계약 유지 후보 없음(§7.4 16단계). 근거는 D-BP26 — 요청당 잔여 Ir이 C 6,447 vs C++ 13,637이고 그 차이가 **지배적 항목 없이 380~830 Ir짜리 10개에 퍼져** 있다(coroutine frame은 516 Ir·3.79%로 격차의 7%에 불과). C에 없는 요청당 할당은 정확히 5개(coroutine frame, reply vector, REQUEST bundle, scheduler 등록 closure, 재개 closure)로 합쳐 약 408 Ir이며, 별개로 C++가 Core `start_async_write`를 2.2배·`start_async_read`를 2.9배 유발해 Core poller wait에서만 922 Ir을 더 쓴다. [pass 1](log/2026-09-07-cpp-multi-reqrep-pass1.ko.md), [pass 2](log/2026-09-07-cpp-multi-reqrep-pass2.ko.md), [비용 지도](log/2026-09-08-cpp-reqrep-cost-map.ko.md) |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 통과(88.1%) | 통과(90.1%) | 통과(88.8%) | 통과(90.3%) | 통과(101.1%) | 미측정 | **`통과(91.67%, 완화 목표 90%)` 확정**; latency 1.323x `통과`(65536 B 2.17x); 개별 최소 85% 미달 없음(최저 64 B 88.05%). 0.17.1의 92.45%와 0.78%p 차이다. 근거는 `MULTI_DEALER_ROUTER_SENDSEND` 행과 같다. 처리량 C++·C 434.5/493.5, 400.9/445.0, 397.5/447.6, 365.7/405.2, 82.1/81.2 Kmsg/s; §7.2 5-run, 고정 Core **0.17.2** prefix(태그 `core/v0.17.2` `dca377aa5e`, Build ID `c5dec25e…`), `q3tcp`, C·C++ 모두 `status: complete`. 0.17.1 값은 참고값이다(D-B216 재고정). |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 보류(76.5%) | 보류(75.5%) | 보류(75.6%) | 보류(78.4%) | 보류(73.2%) | 미측정 | **`보류(75.82%)` 확정**(목표 85%), latency 2.376x(65536 B 7.39x); 처리량 C++·C 277.7/363.2, 259.7/343.8, 251.1/332.2, 238.7/304.5, 46.5/63.6 Kops/s; §7.2 5-run, 고정 Core **0.17.2**(태그 `core/v0.17.2` `dca377aa5e`), `q4tcp`, C·C++ 모두 `status: complete`. **0.17.2가 REQREP을 개선하지 않았다** — 같은 재측정에서 `MULTI_DEALER_DEALER`는 94.73→97.56%(+2.83%p)로 올랐는데 REQREP은 0.3~0.6%p 차이로 사실상 같다. 0.17.2의 completion pull 결함 수정이 REQREP을 느리게 만드는 부분에 닿지 않았다는 뜻이며, **격차가 Core가 아니라 C++ binding의 application 계층에 있다는 D-BP26의 비용 지도와 일치한다**. pass 1·2 완료 후 계약 유지 후보 없음(§7.4 16단계). 근거는 D-BP26 — 요청당 잔여 Ir이 C 6,447 vs C++ 13,637이고 그 차이가 **지배적 항목 없이 380~830 Ir짜리 10개에 퍼져** 있다(coroutine frame은 516 Ir·3.79%로 격차의 7%에 불과). C에 없는 요청당 할당은 정확히 5개(coroutine frame, reply vector, REQUEST bundle, scheduler 등록 closure, 재개 closure)로 합쳐 약 408 Ir이며, 별개로 C++가 Core `start_async_write`를 2.2배·`start_async_read`를 2.9배 유발해 Core poller wait에서만 922 Ir을 더 쓴다. 근거·후보 목록은 `MULTI_DEALER_ROUTER_REQREP` 행과 같다. |
| `tcp` | `MULTI_PUBSUB` | 통과(92.0%) | 통과(92.7%) | 통과(91.8%) | 통과(95.4%) | 통과(104.8%) | 미측정 | **`통과(95.32%)` 확정 — 기본 목표 95% 충족**; latency 1.007x `통과`; 개별 최소 85% 미달 없음(최저 1024 B 91.76%). 0.17.1의 95.16%와 0.16%p 차이로 사실상 동일하다. 처리량 C++·C 2,182.8/2,372.9, 2,244.1/2,420.3, 2,326.3/2,535.3, 1,735.6/1,819.9, 133.4/127.3 Kmsg/s; §7.2 5-run, 고정 Core **0.17.2** prefix(태그 `core/v0.17.2` `dca377aa5e`, Build ID `c5dec25e…`), `q3tcp`, C·C++ 모두 `status: complete`. 0.17.1 값은 참고값이다(D-B216 재고정). |
| `tcp` | `MULTI_STREAM` | 보류 | 보류 | 보류 | 해당 없음 | 보류 | 해당 없음 | **`보류(Core 결함, 0.17.2 대기)`** — 진단 결과 Core 0.17.1의 STREAM 수신 경로에서 TCP로 받은 35 frame분이 packet API로 반환되지 않고 정체한다(양쪽 kernel Recv-Q·Send-Q 0, 수신 436,385 frame vs 반환·송신 436,350 frame, `stop=false`, `recv_packet(DONTWAIT)` probe로도 추가 없음). C++ 송수신·poller를 native C API로 대체한 진단에서도 재현돼 **binding 결함이 아니다**. 트리거는 같은 socket에 recv(pull loop thread)와 send(dispatcher thread)가 동시 진행되는 구성으로 보이며 공개 계약이 금지하지 않는다(`socket/README.ko.md:49`). 러너로 보상하지 않는다(§5). 머신 B에 보고: [Core 결함 보고서](../../../bug/perf/2026-09-08-core-stream-packet-pump-stall.ko.md), D-BP23. 0.17.2에서 재측정한다. [진단](log/2026-09-08-cpp-stream-drain.ko.md) |
| `ws` | `MULTI_DEALER_DEALER` | 통과(85.1%) | 통과(88.0%) | 통과(104.4%) | 통과(99.7%) | 통과(99.1%) | 미측정 | **`통과(95.24%)` 확정 — 기본 목표 95% 충족**; latency 0.645x `통과`; 개별 최소 85% 미달 없음(최저 64 B 85.12%). 처리량 C++·C 1,488.2/1,748.4, 1,276.5/1,451.2, 1,300.9/1,246.4, 639.9/642.0, 171.1/172.7 Kmsg/s; §7.2 5-run, 고정 Core **0.17.2**, `q9ws`, C·C++ 모두 `status: complete`. |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 보류(91.7%) | 보류(91.7%) | 보류(68.1%) | 보류(87.7%) | 보류(97.0%) | 미측정 | **`보류(87.24%)`** — 완화 목표 90%에도 미달; latency 1.616x는 상한 안이나 **1024 B가 처리량 68.10%·latency 3.33x로 무너져** aggregate를 끌어내린다. **간헐 latency 폭증은 C에서도 재현된다**(D-BP29) — C++ 러너 문제가 아니라 server OS TCP 송신 queue에 쌓이는 transport 경계 현상이며 D-BP28의 Core 보고서 §9에 합쳤다. 기준선이 튀는 동안 latency 비율은 판정 입력이 아니다. 처리량 C++·C 410.7/447.9, 390.2/425.3, 239.2/351.3, 227.1/259.1, 28.5/29.4 Kmsg/s; §7.2 5-run, 고정 Core **0.17.2**, `q9ws`, C·C++ 모두 `status: complete`. |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 보류(72.9%) | 보류(72.7%) | 보류(85.5%) | 보류(C 기준 이상) | 보류(C 기준 이상) | 미측정 | **`보류(C 기준 이상)`** — aggregate(DR 99.50%, RR 104.26%)를 판정에 쓰지 않는다. C 기준선이 `ws` 대형 크기에서 정상 성능을 내지 못한다. `MULTI_DEALER_ROUTER_REQREP` 65536 B의 C 값을 transport별로 대조하면: **tcp 63,265 ops/s·0.808 ms, tls 41,345·2.320 ms, ws 25,849·0.086 ms**. `ws`는 처리량이 tcp의 41%로 떨어졌는데 **latency는 오히려 10배 낮다** — 부하를 걸지 못하고 한 건씩 왕복하는 상태이며 정상 기준선이 아니다. 그 결과 C++가 4096 B 131~132%, 65536 B 135~155%로 보이고 aggregate가 100% 안팎으로 부풀었다. 2026-09-05에 같은 성격을 `보류(C 기준 이상)`로 기록하고 C 러너를 고친 적이 있으나(D-B89, `ws`·`wss` REQREP **4096 B**), **65536 B에 같은 문제가 남아 있다.** **Core/transport 문제로 확인됐다**(D-BP28) — 독립 구현인 C·C++ 러너가 같은 곡선(단방향 ws/tcp 1.0, 왕복 0.36~0.40)을 그린다. 머신 B에 보고: [Core 결함 보고서](../../../bug/perf/2026-09-08-core-ws-roundtrip-size-penalty.ko.md). 수정 전까지 이 셀은 판정하지 않는다. 작은 크기(64·256 B)는 `tcp`·`tls`와 같은 수준이다 — DR 72.91/72.69%, RR 76.44/73.74%로 D-BP26의 결론과 일치한다. 처리량 C++·C 290.3/398.1, 267.4/367.9, 262.3/306.8, 239.8/183.0, 35.0/25.8 Kops/s; §7.2 5-run, 고정 Core **0.17.2**, `qBrq`, C·C++ 모두 `status: complete` |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 보류(87.2%) | 보류(98.8%) | 보류(87.7%) | 보류(90.5%) | 보류(97.3%) | 미측정 | **`보류(C 기준 이상 — latency)`** throughput 92.29%(완화 목표 초과) — throughput은 완화 목표 90%를 넘지만 **64 B latency가 258.39x**로 §2.2 상한을 크게 넘어 aggregate latency가 52.32x다. **간헐 latency 폭증은 C에서도 재현된다**(D-BP29) — C++ 러너 문제가 아니라 server OS TCP 송신 queue에 쌓이는 transport 경계 현상이며 D-BP28의 Core 보고서 §9에 합쳤다. 기준선이 튀는 동안 latency 비율은 판정 입력이 아니다. 처리량 C++·C 394.6/452.8, 378.0/382.7, 256.0/292.0, 141.3/156.1, 27.8/28.6 Kmsg/s; §7.2 5-run, 고정 Core **0.17.2**, `q9ws`, C·C++ 모두 `status: complete`. |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 보류(76.4%) | 보류(73.7%) | 보류(84.3%) | 보류(C 기준 이상) | 보류(C 기준 이상) | 미측정 | **`보류(C 기준 이상)`** — aggregate(DR 99.50%, RR 104.26%)를 판정에 쓰지 않는다. C 기준선이 `ws` 대형 크기에서 정상 성능을 내지 못한다. `MULTI_DEALER_ROUTER_REQREP` 65536 B의 C 값을 transport별로 대조하면: **tcp 63,265 ops/s·0.808 ms, tls 41,345·2.320 ms, ws 25,849·0.086 ms**. `ws`는 처리량이 tcp의 41%로 떨어졌는데 **latency는 오히려 10배 낮다** — 부하를 걸지 못하고 한 건씩 왕복하는 상태이며 정상 기준선이 아니다. 그 결과 C++가 4096 B 131~132%, 65536 B 135~155%로 보이고 aggregate가 100% 안팎으로 부풀었다. 2026-09-05에 같은 성격을 `보류(C 기준 이상)`로 기록하고 C 러너를 고친 적이 있으나(D-B89, `ws`·`wss` REQREP **4096 B**), **65536 B에 같은 문제가 남아 있다.** **Core/transport 문제로 확인됐다**(D-BP28) — 독립 구현인 C·C++ 러너가 같은 곡선(단방향 ws/tcp 1.0, 왕복 0.36~0.40)을 그린다. 머신 B에 보고: [Core 결함 보고서](../../../bug/perf/2026-09-08-core-ws-roundtrip-size-penalty.ko.md). 수정 전까지 이 셀은 판정하지 않는다. 작은 크기(64·256 B)는 `tcp`·`tls`와 같은 수준이다 — DR 72.91/72.69%, RR 76.44/73.74%로 D-BP26의 결론과 일치한다. 근거는 `MULTI_DEALER_ROUTER_REQREP` 행과 같다. 처리량 C++·C 272.4/356.3, 250.1/339.1, 246.9/292.8, 231.1/174.8, 38.1/24.6 Kops/s; §7.2 5-run, 고정 Core **0.17.2**, `qBrq`, C·C++ 모두 `status: complete` |
| `ws` | `MULTI_PUBSUB` | 통과(93.8%) | 통과(99.5%) | 통과(87.0%) | 통과(100.9%) | 통과(96.1%) | 미측정 | **`통과(95.44%)` 확정 — 기본 목표 95% 충족**; latency 1.029x `통과`; 개별 최소 85% 미달 없음(최저 1024 B 86.95%). 처리량 C++·C 567.5/605.3, 855.8/859.9, 1,042.4/1,198.8, 1,304.2/1,293.2, 109.3/113.7 Kmsg/s; §7.2 5-run, 고정 Core **0.17.2**, `q9ws`, C·C++ 모두 `status: complete`. |
| `ws` | `MULTI_STREAM` | 보류 | 보류 | 보류 | 해당 없음 | 보류 | 해당 없음 | **`보류(Core 결함, 0.17.2 대기)`** — 진단 결과 Core 0.17.1의 STREAM 수신 경로에서 TCP로 받은 35 frame분이 packet API로 반환되지 않고 정체한다(양쪽 kernel Recv-Q·Send-Q 0, 수신 436,385 frame vs 반환·송신 436,350 frame, `stop=false`, `recv_packet(DONTWAIT)` probe로도 추가 없음). C++ 송수신·poller를 native C API로 대체한 진단에서도 재현돼 **binding 결함이 아니다**. 트리거는 같은 socket에 recv(pull loop thread)와 send(dispatcher thread)가 동시 진행되는 구성으로 보이며 공개 계약이 금지하지 않는다(`socket/README.ko.md:49`). 러너로 보상하지 않는다(§5). 머신 B에 보고: [Core 결함 보고서](../../../bug/perf/2026-09-08-core-stream-packet-pump-stall.ko.md), D-BP23. 0.17.2에서 재측정한다. [진단](log/2026-09-08-cpp-stream-drain.ko.md) |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | **측정 차단 — C 기준 러너가 `wss` 4096 B에서 간헐 실패**(`server_non_zero_exit_1`, `qAwss` C report `status: partial`). C++ report는 `complete`지만 짝이 없어 판정할 수 없다. D-BP25(C `ctx_term` 누락, 수정 완료)와는 다른 지점이다. **깨끗한 재현은 1승 1패**(`qAwss` 4096 B 실패 / `wssdd_full` 4096 B 단독 통과)이고, 전 크기 3회 재현(`wssdd_x1~3`)은 C++ latency 조사의 측정과 1초 차이로 겹쳐 **오염돼 무효**다 — idle 가드는 대기자 여럿이 동시에 깨어나면 함께 출발한다. perf가 조용할 때 다시 잰다. `header mismatch` 디버그 줄은 빈 tail part마다 찍히는 정상 노이즈이며 원인이 아니다(`decode_payload_header`가 먼저 false를 돌려주고 stale header를 출력). |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 보류(94.5%) | 보류(86.3%) | 보류(86.8%) | 보류(101.2%) | 보류(93.6%) | 미측정 | **`보류(C 기준 이상 — latency)`** throughput 92.46%(완화 목표 초과) — throughput은 완화 목표 90%를 넘지만 **64 B 23.55x·256 B 183.55x**로 aggregate latency가 42.05x다(§2.2 상한 2.0x). **간헐 latency 폭증은 C에서도 재현된다**(D-BP29) — C++ 러너 문제가 아니라 server OS TCP 송신 queue에 766 MB까지 쌓이는 transport 경계 현상이며 D-BP28의 Core 보고서 §9에 합쳤다. `tcp`에서는 안 난다. 기준선이 튀는 동안 latency 비율은 판정 입력이 아니다. 처리량 C++·C 409.8/433.9, 352.7/408.9, 264.3/304.7, 116.4/115.0, 26.5/28.3 Kmsg/s; §7.2 5-run, 고정 Core **0.17.2**, `qAwss`, C·C++ 모두 `status: complete`. |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 보류(75.2%) | 보류(74.3%) | 보류(82.1%) | 보류(C 기준 이상) | 보류(C 기준 이상) | 미측정 | **`보류(C 기준 이상)`** — `ws`와 같은 Core/transport 문제(D-BP28, [보고서](../../../bug/perf/2026-09-08-core-ws-roundtrip-size-penalty.ko.md))가 `wss`에도 있다. 대형 크기에서 C 기준선이 무너져(65536 B C 23,988 ops/s vs tcp 63,265) 4096 B가 96~105%로 부풀고 65536 B latency가 240~332x다. aggregate를 판정에 쓰지 않는다. 작은 크기(64·256 B)는 `tcp`·`tls`와 같은 수준이다 — DR 75.15/74.28%, RR 73.60/72.80%로 D-BP26의 결론과 일치한다. 수정 전까지 판정하지 않는다. 처리량 C++·C 272.1/362.1, 248.6/334.6, 240.8/293.3, 167.4/173.5, 16.9/24.0 Kops/s; §7.2 5-run, 고정 Core **0.17.2**, `qCrq`, C·C++ 모두 `status: complete` |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 보류(90.6%) | 보류(89.2%) | 보류(84.8%) | 보류(90.9%) | 보류(98.2%) | 미측정 | **`보류(C 기준 이상 — latency)`** throughput 90.74%(완화 목표 초과) — **64 B 17.58x·256 B 125.59x**로 aggregate latency 29.28x다. **간헐 latency 폭증은 C에서도 재현된다**(D-BP29) — C++ 러너 문제가 아니라 server OS TCP 송신 queue에 766 MB까지 쌓이는 transport 경계 현상이며 D-BP28의 Core 보고서 §9에 합쳤다. `tcp`에서는 안 난다. 기준선이 튀는 동안 latency 비율은 판정 입력이 아니다. 처리량 C++·C 401.2/443.1, 368.7/413.5, 280.6/330.8, 113.9/125.3, 27.9/28.4 Kmsg/s; §7.2 5-run, 고정 Core **0.17.2**, `qAwss`, C·C++ 모두 `status: complete`. |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 보류(73.6%) | 보류(72.8%) | 보류(82.4%) | 보류(C 기준 이상) | 보류(C 기준 이상) | 미측정 | **`보류(C 기준 이상)`** — `ws`와 같은 Core/transport 문제(D-BP28, [보고서](../../../bug/perf/2026-09-08-core-ws-roundtrip-size-penalty.ko.md))가 `wss`에도 있다. 대형 크기에서 C 기준선이 무너져(65536 B C 23,988 ops/s vs tcp 63,265) 4096 B가 96~105%로 부풀고 65536 B latency가 240~332x다. aggregate를 판정에 쓰지 않는다. 작은 크기(64·256 B)는 `tcp`·`tls`와 같은 수준이다 — DR 75.15/74.28%, RR 73.60/72.80%로 D-BP26의 결론과 일치한다. 수정 전까지 판정하지 않는다. 근거는 `MULTI_DEALER_ROUTER_REQREP` 행과 같다. 처리량 C++·C 242.5/329.5, 224.2/307.9, 222.8/270.3, 171.6/162.7, 17.1/22.1 Kops/s; §7.2 5-run, 고정 Core **0.17.2**, `qCrq`, C·C++ 모두 `status: complete` |
| `wss` | `MULTI_PUBSUB` | 통과(91.2%) | 통과(101.2%) | 통과(95.4%) | 통과(92.6%) | 통과(95.3%) | 미측정 | **`통과(95.15%)` 확정 — 기본 목표 95% 충족**; latency 1.037x `통과`; 개별 최소 85% 미달 없음(최저 64 B 91.19%). 처리량 C++·C 1,167.8/1,280.6, 1,547.2/1,528.3, 1,463.3/1,533.5, 748.8/808.5, 84.9/89.1 Kmsg/s; §7.2 5-run, 고정 Core **0.17.2**, `qAwss`, C·C++ 모두 `status: complete`. |
| `wss` | `MULTI_STREAM` | 보류 | 보류 | 보류 | 해당 없음 | 보류 | 해당 없음 | **`보류(Core 결함, 0.17.2 대기)`** — 진단 결과 Core 0.17.1의 STREAM 수신 경로에서 TCP로 받은 35 frame분이 packet API로 반환되지 않고 정체한다(양쪽 kernel Recv-Q·Send-Q 0, 수신 436,385 frame vs 반환·송신 436,350 frame, `stop=false`, `recv_packet(DONTWAIT)` probe로도 추가 없음). C++ 송수신·poller를 native C API로 대체한 진단에서도 재현돼 **binding 결함이 아니다**. 트리거는 같은 socket에 recv(pull loop thread)와 send(dispatcher thread)가 동시 진행되는 구성으로 보이며 공개 계약이 금지하지 않는다(`socket/README.ko.md:49`). 러너로 보상하지 않는다(§5). 머신 B에 보고: [Core 결함 보고서](../../../bug/perf/2026-09-08-core-stream-packet-pump-stall.ko.md), D-BP23. 0.17.2에서 재측정한다. [진단](log/2026-09-08-cpp-stream-drain.ko.md) |
| `tls` | `MULTI_DEALER_DEALER` | 통과(85.2%) | 통과(90.9%) | 통과(98.5%) | 통과(104.8%) | 통과(99.1%) | 미측정 | **`통과(95.69%)` 확정 — 기본 목표 95% 충족**; latency 0.794x `통과`; 개별 최소 85% 미달 없음(최저 64 B 85.24%). 0.17.1의 95.55%와 0.14%p 차이로 사실상 동일하다. 처리량 C++·C 1,492.7/1,751.2, 1,296.9/1,427.0, 1,176.0/1,194.1, 822.6/785.1, 104.4/105.4 Kmsg/s; §7.2 5-run, 고정 Core **0.17.2**(태그 `core/v0.17.2` `dca377aa5e`), `q5tls`, C·C++ 모두 `status: complete`. |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 통과(90.9%) | 통과(93.9%) | 통과(90.2%) | 통과(93.1%) | 통과(106.7%) | 미측정 | **`통과(94.95%, 완화 목표 90%)` 확정**; latency 1.287x `통과`; 개별 최소 85% 미달 없음(최저 1024 B 90.20%). 기본 목표 95%에 0.05%p 모자란다. 완화 근거는 D-BP22·D-BP26으로 `tcp` 행과 같다. **러너 결함 3건을 해소한 뒤 처음으로 측정된 셀이다**(relay 직렬화 `e0862e1e5c`, client echo drain `33f63ae89d`, C `ctx_term` `1aa2751b1b`; D-BP24·D-BP25). 처리량 C++·C 404.1/444.7, 381.0/405.6, 368.9/409.0, 180.3/193.7, 36.6/34.3 Kmsg/s; §7.2 5-run, 고정 Core **0.17.2**(태그 `core/v0.17.2` `dca377aa5e`), `q5tls`, C·C++ 모두 `status: complete`. |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 보류(74.5%) | 보류(76.7%) | 보류(75.0%) | 보류(88.4%) | 보류(52.0%) | 미측정 | **`보류(73.32%)` 확정**(목표 85%), latency 4.119x(65536 B 17.78x가 끌어올린다); 개별 최소 75% 미달 65536 B(52.02%); 처리량 C++·C 274.3/368.0, 258.3/336.9, 248.2/330.9, 229.8/260.1, 21.5/41.3 Kops/s; §7.2 5-run, 고정 Core **0.17.2**, `q8tls`, C·C++ 모두 `status: complete`. **§10.3.1대로 개선 pass를 새로 열지 않았다** — D-BP26의 비용 지도가 확정한 원인(요청당 고정 비용이 지배적 항목 없이 380~830 Ir짜리 10개에 퍼져 있음)은 transport를 모르고, 같은 재측정에서 0.17.2가 `tcp` REQREP을 전혀 개선하지 못했다(DD는 +2.83%p, REQREP은 0.3~0.6%p). **`tcp`와 값이 같다는 것이 그 전제의 측정 확인이다** — DR 73.32 vs 72.61%, RR 76.12 vs 75.82%. pass 1·2는 `tcp` 측정으로 수행했고 남은 후보는 모두 공개 계약 변경이다. [pass 1](log/2026-09-07-cpp-multi-reqrep-pass1.ko.md), [pass 2](log/2026-09-07-cpp-multi-reqrep-pass2.ko.md), [비용 지도](log/2026-09-08-cpp-reqrep-cost-map.ko.md) |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 보류(88.5%) | 보류(92.8%) | 보류(87.6%) | 보류(93.4%) | 보류(98.8%) | 미측정 | **`보류(C 기준 이상 — latency)`** throughput 91.24%(완화 목표 초과) — throughput은 완화 목표 90%를 넘지만(5-run 3회: 88.58·91.24·92.22%, 중앙값 91.24%) **1024 B latency가 간헐적으로 폭증해 §2.2의 2.0x 상한을 넘는다**. 5회 측정 중 3회 발생: `pDtls5` 322x, `pFtls5` 180x(둘 다 0.17.1), `q5tls` 2.99x, `q6tls` 0.98x, `q7tls` **104x**(셋 다 0.17.2). aggregate latency는 각각 65.5x·37.0x·1.61x·1.21x·21.7x다. **0.17.2에서도 사라지지 않았다** — q5tls·q6tls가 연속 정상이어서 한때 해소된 것으로 판단했으나 q7tls에서 재발했다. 원값 예: C++ 69.2~114.3 ms vs C 0.35~0.38 ms. 깊이로 환산하면 1024 B에서만 약 24,600건이 파이프라인에 쌓이고 이웃 크기는 수백 건이다 — 점진적 증가가 아니라 그 크기에서만 일어나는 질적 전환이다. **relay 직렬화 수정(`e0862e1e5c`)이 원인이라는 가설은 A/B로 기각했다**(D-BP27: `tcp` RR 1024 B는 수정 전후 모두 1.09x). throughput만 보면 `통과(완화)`지만 latency 상한을 만족하지 못해 보류로 둔다. 조사 항목. 처리량 C++·C 396.6/448.3, 377.9/407.4, 359.0/409.7, 119.2/127.5, 34.2/34.6 Kmsg/s; §7.2 5-run ×3, 고정 Core **0.17.2**, `q5tls`·`q6tls`·`q7tls`, 모두 `status: complete` |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 보류(76.0%) | 보류(75.3%) | 보류(76.8%) | 보류(81.6%) | 보류(70.9%) | 미측정 | **`보류(76.12%)` 확정**(목표 85%), latency 3.096x(65536 B 12.20x); 개별 최소 75% 미달 65536 B(70.90%); 처리량 C++·C 260.8/343.0, 238.6/316.9, 234.5/305.4, 209.3/256.4, 22.4/31.6 Kops/s; §7.2 5-run, 고정 Core **0.17.2**, `q8tls`, C·C++ 모두 `status: complete`. **§10.3.1대로 개선 pass를 새로 열지 않았다** — D-BP26의 비용 지도가 확정한 원인(요청당 고정 비용이 지배적 항목 없이 380~830 Ir짜리 10개에 퍼져 있음)은 transport를 모르고, 같은 재측정에서 0.17.2가 `tcp` REQREP을 전혀 개선하지 못했다(DD는 +2.83%p, REQREP은 0.3~0.6%p). **`tcp`와 값이 같다는 것이 그 전제의 측정 확인이다** — DR 73.32 vs 72.61%, RR 76.12 vs 75.82%. pass 1·2는 `tcp` 측정으로 수행했고 남은 후보는 모두 공개 계약 변경이다. 근거·후보 목록은 `MULTI_DEALER_ROUTER_REQREP` 행과 같다. |
| `tls` | `MULTI_PUBSUB` | 통과(96.3%) | 통과(105.0%) | 통과(90.5%) | 통과(93.2%) | 통과(88.1%) | 미측정 | **`통과(94.61%, 완화 목표 90%)` 확정**; latency 1.026x `통과`; 개별 최소 85% 미달 없음(최저 65536 B 88.07%). **이 셀은 run 간 변동이 크다** — 0.17.2에서 5-run 두 차례가 92.23%와 94.61%이고 256 B 셀이 119.36→85.48→105.01%로 진동한다(bimodal). 다만 **두 측정의 판정이 같아**(기본 95% 미달·완화 90% 초과) 판정은 확정한다. 0.17.1의 97.40%는 다른 Core라 비교 대상이 아니다. 완화 근거: PUBSUB은 2026-09-05 자체 pass 1과 Sol 리뷰 pass 2 모두 계약 유지 후보 없음(no-go)으로 끝났다. 처리량 C++·C 750.6/779.3, 1,311.2/1,248.6, 1,523.9/1,684.2, 841.2/903.1, 88.4/100.4 Kmsg/s; §7.2 5-run, 고정 Core **0.17.2**, `q6tls`(재확인)·`q5tls`(1차), C·C++ 모두 `status: complete` |
| `tls` | `MULTI_STREAM` | 보류 | 보류 | 보류 | 해당 없음 | 보류 | 해당 없음 | **`보류(Core 결함, 0.17.2 대기)`** — 진단 결과 Core 0.17.1의 STREAM 수신 경로에서 TCP로 받은 35 frame분이 packet API로 반환되지 않고 정체한다(양쪽 kernel Recv-Q·Send-Q 0, 수신 436,385 frame vs 반환·송신 436,350 frame, `stop=false`, `recv_packet(DONTWAIT)` probe로도 추가 없음). C++ 송수신·poller를 native C API로 대체한 진단에서도 재현돼 **binding 결함이 아니다**. 트리거는 같은 socket에 recv(pull loop thread)와 send(dispatcher thread)가 동시 진행되는 구성으로 보이며 공개 계약이 금지하지 않는다(`socket/README.ko.md:49`). 러너로 보상하지 않는다(§5). 머신 B에 보고: [Core 결함 보고서](../../../bug/perf/2026-09-08-core-stream-packet-pump-stall.ko.md), D-BP23. 0.17.2에서 재측정한다. [진단](log/2026-09-08-cpp-stream-drain.ko.md) |

### 9.2 .NET

- perf 경로: `bindings/dotnet/perf`
- Single 상태: `미측정`
- Multi 상태: `보류` — Core `a40cb46335`+pass 3 재판정(quiet 3-run 20:04 KST): DD 59.6%, DR 58.3%, RR 67.0%, PUBSUB 61.3% 모두 `보류`(64 KiB 셀 106→114~131%로 회복; 15:07 판정 61.1/58.5/68.0/56.9)(before 44.7/51.6/53.6/44.8; pass 1 `b4fc201f7e`, pass 2 `5dc05bfb3e`; §7.5 두 pass 완료·계약 유지 후보 소진); tls/ws/wss·Single 미측정(다음 언어 뒤 회귀); [log](log/2026-09-05-dotnet-multi-tcp-before.ko.md)
- 다음 작업: inventory gate에서 확인한 pattern으로 paired 측정을 시작한다.

#### 9.2.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `tcp` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ws` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ws` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `wss` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `wss` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `tls` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `tls` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `inproc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `inproc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ipc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ipc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |

#### 9.2.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 미달(53.0%) | 미달(54.4%) | 미달(59.2%) | 미달(87.8%) | 미달(58.1%) | 미측정 | **`미달(62.51%)`** — §7.2 5-run, 고정 Core **0.17.2**(`r1net`, C·.NET 모두 `status: complete`); latency 1.614x `통과`. 0.17.1 참고값 59.6%와 같은 수준. §2.1의 과거 p10(74.9%)에도 못 미치므로 어떤 기준으로도 미달이다. **C++ 같은 셀(97.56%)과 격차의 규모가 다르다** — C++는 5~8%p로 D-BP26이 '지배적 항목 없이 퍼진 고정 비용'으로 확정했지만 .NET은 38%p라 지배적 비용(GC·P/Invoke 후보)이 있을 가능성이 크다. 이전 pass 1·2(`b4fc201f7e`·`5dc05bfb3e`, D-B108)는 REQREP 중심이었고 DD 자체 비용 지도는 없었다. **측정 전용 비용 지도 job 진행 중**([기록](log/2026-09-08-dotnet-dd-cost-map.ko.md)) — 지도가 나온 뒤 개선 pass 여부를 판단한다. 4096 B만 87.78%로 유독 높은 이유도 지도에서 확인한다. 처리량 .NET·C 922.8/1,741.2, 783.5/1,440.0, 707.5/1,194.7, 575.8/655.9, 94.4/162.4 Kmsg/s; C `perf_c_multi_linux_20260908_081717_r1net.txt`, .NET `perf_dotnet_multi_linux_20260908_081958_r1net.txt` |
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 보류(35.4%) | 보류(40.5%) | 보류(49.3%) | 보류(54.4%) | 보류(113.0%) | 미측정 | 판정 확정 `보류(58.5%)`(목표 70%; before 51.6), latency 0.52x; 처리량 .NET/C 70.9/200.3, 69.6/171.7, 70.7/143.3, 68.5/126.0, 26.0/23.0 Kops/s; [log](log/2026-09-05-dotnet-multi-tcp-before.ko.md) |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 보류(40.4%) | 보류(56.5%) | 보류(55.7%) | 보류(63.1%) | 보류(124.6%) | 미측정 | 판정 확정 `보류(68.0%)`(목표 70%; before 53.6), latency 0.52x; 처리량 .NET/C 68.0/168.4, 69.8/123.5, 67.4/121.1, 65.2/103.4, 24.8/19.9 Kops/s; [log](log/2026-09-05-dotnet-multi-tcp-before.ko.md) |
| `tcp` | `MULTI_PUBSUB` | 미달(43.5%) | 미달(47.4%) | 미달(44.4%) | 미달(67.7%) | 미달(102.2%) | 미측정 | **`미달(61.01%)`** — §7.2 5-run, 고정 Core **0.17.2**(`r1net`, C·.NET 모두 `status: complete`); latency 1.385x `통과`. 0.17.1 참고값 61.3%와 같다. DD(62.51%)와 같은 규모이며 **64/256/1024 B가 43~47%로 더 낮고 65536 B는 102%** — 작은 메시지의 메시지당 오버헤드가 지배적이라는 신호로 DD와 같은 원인(GC·P/Invoke 후보)일 가능성이 크다. DD 비용 지도([기록](log/2026-09-08-dotnet-dd-cost-map.ko.md))가 나온 뒤 함께 판단한다. 처리량 .NET·C 1,021.9/2,351.2, 1,139.5/2,405.3, 1,106.8/2,494.8, 1,085.6/1,603.8, 118.5/116.0 Kmsg/s; C `perf_c_multi_linux_20260908_082221_r1net.txt`, .NET `perf_dotnet_multi_linux_20260908_082458_r1net.txt` |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |

### 9.3 Java

- perf 경로: `bindings/java/perf`
- Single 상태: `미측정`
- Multi 상태: `미달/통과` — before 11:44 → pass 1+1b(`82b0fd9f38`: REQREP 일괄 settlement, Context당 completion pump 1개) → 3-run 15:23: DD 70.8/DR 54.7/RR 58.5/PUBSUB 102.9 → pass 2(`6402fcabc4`)+Core `a40cb46335` 재판정 3-run 20:18: DD 80.9%(목표 90, 최소 70 충족), DR 59.4%(70), RR 58.7%(70), PUBSUB 80.9%(90; lossy 변동) `미달`; [log](log/2026-09-05-java-multi-tcp-before.ko.md)
- 다음 작업: inventory gate에서 확인한 pattern으로 paired 측정을 시작한다.

#### 9.3.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `tcp` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ws` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ws` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `wss` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `wss` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `tls` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `tls` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `inproc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `inproc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ipc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ipc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |

#### 9.3.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 미달(75.9%) | 미달(72.9%) | 미달(77.0%) | 미달(66.1%) | 미달(62.2%) | 미측정 | pass 1+1b(`82b0fd9f38`) 뒤 3-run `p1java-r3q`: aggregate **70.8%**(목표 90%, 최소 70% 충족; before 50.8), latency 0.47x; 처리량 Java/C 757.0/998.0, 691.0/947.2, 606.7/787.6, 198.6/300.4, 39.9/64.2 Kmsg/s; 리뷰 pass 2 전; [log](log/2026-09-05-java-multi-tcp-before.ko.md) |
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 미달(37.0%) | 미달(38.5%) | 미달(44.8%) | 미달(44.4%) | 미달(108.8%) | 미측정 | pass 1+1b 뒤 3-run: **54.7%**(목표 70%; before 15.1), latency 0.64x; 처리량 Java/C 71.2/192.3, 64.1/166.6, 75.2/168.0, 57.1/128.5, 24.5/22.5 Kops/s; 리뷰 pass 2 전; [log](log/2026-09-05-java-multi-tcp-before.ko.md) |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 미달(39.8%) | 미달(31.3%) | 미달(58.2%) | 미달(68.3%) | 미달(95.0%) | 미측정 | pass 1+1b 뒤 3-run: **58.5%**(목표 70%; before 24.0), latency 0.73x; 처리량 Java/C 70.7/177.5, 40.3/128.6, 72.8/125.1, 71.3/104.4, 19.7/20.8 Kops/s; 리뷰 pass 2 전; [log](log/2026-09-05-java-multi-tcp-before.ko.md) |
| `tcp` | `MULTI_PUBSUB` | 통과(102.5%) | 통과(101.3%) | 통과(97.4%) | 통과(83.9%) | 통과(129.2%) | 미측정 | pass 1+1b 뒤 3-run: **102.9%** `통과`(목표 90%; before 81.0), latency 0.99x; 처리량 Java/C 583.5/569.2, 668.0/659.4, 709.5/728.5, 485.4/578.8, 74.9/58.0 Kmsg/s; [log](log/2026-09-05-java-multi-tcp-before.ko.md) |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |

### 9.4 Node

- perf 경로: `bindings/node/perf`
- Single 상태: `미측정`
- Multi 상태: `미달` — before 13:53 → pass 1(`f69558af55`: multipart Buffer 경로) DD 27.6→41.2% → pass 1b(`6c2cb784c0`: 공유 pump 기각, N-API callback scope) → pass 1c(`8ec82c4be7`: 서버 수신 metadata·reply Buffer) DR/RR REQREP 18.7/17.9→28.7/31.1%, PUBSUB 34.8%. REQREP는 요청당 ~60 ms 고정 지연·timeout 35~55%가 남음(요청 수명 계측: 서버 TCP 수신→ROUTER recv 128 ms 구간; 클라이언트 WRITABLE 재제출 루프 의심 → Core 조사 D-B117). Core `a40cb46335` quiet 3-run 22:10(4 pattern): DD 35.9%(목표 60, 최소 35 충족), DR/RR 24.3/24.8%(60), PUBSUB 30.2%(60) → pass 1d(D-B129): 지연의 98%가 서버 입력 backlog(Node ROUTER 요청당 JS 처리 ~40 µs), client wake 가설 기각 → **`보류`**(4 pass, 계약 유지 후보 소진; 다음 방향은 native 배치 recv+reply 공개 API = spec 결정); [log](log/2026-09-05-node-multi-tcp-before.ko.md)
- 다음 작업: inventory gate에서 확인한 pattern으로 paired 측정을 시작한다.

#### 9.4.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `tcp` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ws` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ws` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `wss` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `wss` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `tls` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `tls` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `inproc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `inproc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ipc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ipc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |

#### 9.4.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 보류(15.8%) | 보류(17.5%) | 보류(20.4%) | 보류(39.9%) | 보류(44.2%) | 미측정 | 판정 `보류`(4 pass 뒤, D-B129; quiet 3-run 22:10 값은 log) — before; aggregate 27.6%(목표 60%), latency 204x(큐 깊이, 판정 제외); 처리량 Node/C 160.7/1017.4, 166.7/955.0, 169.7/832.4, 126.0/315.6, 26.8/60.7 Kmsg/s; `p1node`; Node `perf_node_multi_linux_20260905_135524_p1node.txt`; [log](log/2026-09-05-node-multi-tcp-before.ko.md) |
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 보류(7.6%) | 보류(6.2%) | 보류(5.3%) | 보류(9.7%) | 보류(64.9%) | 미측정 | 판정 `보류`(4 pass 뒤, D-B129; quiet 3-run 22:10 값은 log) — before; aggregate 18.7%(목표 60%), latency 42x; 8~15k ops/s 고정; 처리량 Node/C 11.0/145.3, 10.1/163.5, 8.3/155.3, 11.9/122.9, 14.4/22.2 Kops/s; Node `..._135649_p1node.txt`; [log](log/2026-09-05-node-multi-tcp-before.ko.md) |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 보류(5.5%) | 보류(5.2%) | 보류(6.0%) | 보류(13.0%) | 보류(59.8%) | 미측정 | 판정 `보류`(4 pass 뒤, D-B129; quiet 3-run 22:10 값은 log) — before; aggregate 17.9%(목표 60%), latency 55x; 처리량 Node/C 9.7/176.4, 7.5/144.4, 8.4/139.6, 14.8/113.7, 12.9/21.5 Kops/s; Node `..._135755_p1node.txt`; [log](log/2026-09-05-node-multi-tcp-before.ko.md) |
| `tcp` | `MULTI_PUBSUB` | 보류(19.0%) | 보류(21.2%) | 보류(20.1%) | 보류(22.6%) | 보류(60.1%) | 미측정 | 판정 `보류`(4 pass 뒤, D-B129; quiet 3-run 22:10 값은 log) — before; aggregate 28.6%(목표 60%), latency 0.91x; 처리량 Node/C 111.7/588.1, 164.8/778.0, 170.7/849.2, 146.2/646.5, 41.1/68.4 Kmsg/s; Node `..._135859_p1node.txt`; [log](log/2026-09-05-node-multi-tcp-before.ko.md) |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |

### 9.5 Go

- perf 경로: `bindings/go/perf`
- Single 상태: `미측정`
- Multi 상태: `미달` — before 17:46: DD 31.5%, REQREP 2.9/2.8%, PUBSUB 47.3% → pass 1(`5e7e68fa6a`)+1b(`53ba0d04a7`: REQREP 러너 socket당 goroutine) quiet 3-run 21:36: DD 53.2%(목표 65), DR/RR 19.0/21.8%(53; 요청 1건 in-flight 모델, D-B127 대기), PUBSUB 54.6%(65); [log](log/2026-09-05-go-multi-tcp-before.ko.md)
- 다음 작업: inventory gate에서 확인한 pattern으로 paired 측정을 시작한다.

#### 9.5.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `tcp` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ws` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ws` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `wss` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `wss` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `tls` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `tls` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `inproc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `inproc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ipc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ipc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |

#### 9.5.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 미달(20.2%) | 미달(24.0%) | 미달(22.1%) | 미달(50.3%) | 미달(40.9%) | 미측정 | before(참고: Core job 빌드와 겹침); aggregate 31.5%(목표 65%), latency 43x(큐); 처리량 Go/C 202.9/1003.8, 190.2/793.1, 106.5/481.3, 82.8/164.8, 18.4/45.1 Kmsg/s; `p1go`; [log](log/2026-09-05-go-multi-tcp-before.ko.md) |
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 미달(1.4%) | 미달(1.2%) | 미달(1.6%) | 미달(1.6%) | 미달(9.0%) | 미측정 | before; aggregate 2.9%(목표 53%), latency 0.11x; 1.1~1.5k ops/s 고정(요청당 ~70 ms); 처리량 Go/C 1.5/112.5, 1.4/120.0, 1.5/97.4, 1.4/92.7, 1.2/13.3 Kops/s; [log](log/2026-09-05-go-multi-tcp-before.ko.md) |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 미달(1.4%) | 미달(1.3%) | 미달(1.4%) | 미달(1.7%) | 미달(8.0%) | 미측정 | before; aggregate 2.8%(목표 53%), latency 0.20x; 처리량 Go/C 1.4/97.0, 1.3/99.4, 1.2/90.2, 1.3/73.7, 1.1/13.3 Kops/s; [log](log/2026-09-05-go-multi-tcp-before.ko.md) |
| `tcp` | `MULTI_PUBSUB` | 미달(31.0%) | 미달(29.0%) | 미달(33.9%) | 미달(32.4%) | 미달(110.0%) | 미측정 | before; aggregate 47.3%(목표 65%), latency 1.73x; 처리량 Go/C 148.4/479.4, 150.8/519.8, 189.7/559.2, 158.5/489.0, 52.2/47.5 Kmsg/s; [log](log/2026-09-05-go-multi-tcp-before.ko.md) |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |

### 9.6 Rust

- perf 경로: `bindings/rust/perf`
- Single 상태: `미측정`
- Multi 상태: `보류` — before 19:28: DD 63.0%, REQREP 65.9/68.2%, PUBSUB 83.2% → pass 1(`4163072701`)·pass 2(`ddb614faf9`) 뒤 quiet 3-run 09-06 00:12: DD 59.3%(목표 95), DR/RR 67.3/69.8%(85), PUBSUB 87.7%(95, 최소 85 충족) — 두 pass 완료·계약 유지 후보 소진으로 `보류` 확정(§7.5); [log](log/2026-09-05-rust-multi-tcp-before.ko.md)
- 다음 작업: inventory gate에서 확인한 pattern으로 paired 측정을 시작한다.

#### 9.6.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `tcp` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ws` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ws` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `wss` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `wss` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `tls` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `tls` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `inproc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `inproc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ipc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ipc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |

#### 9.6.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 보류(43.6%) | 보류(46.7%) | 보류(47.8%) | 보류(76.7%) | 보류(100.1%) | 미측정 | 판정 `보류`(pass 1·2 뒤 3-run 값은 log) — before; aggregate 63.0%(목표 95%), latency 34x(큐); 처리량 Rust/C 494.7/1134.4, 498.5/1066.7, 411.0/859.7, 205.3/267.8, 56.6/56.5 Kmsg/s; `p1rust`; [log](log/2026-09-05-rust-multi-tcp-before.ko.md) |
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 보류(67.8%) | 보류(63.5%) | 보류(70.0%) | 보류(65.8%) | 보류(62.4%) | 미측정 | 판정 `보류`(pass 1·2 뒤 3-run 값은 log) — before; aggregate 65.9%(목표 85%), latency 1.43x; 처리량 Rust/C 108.6/160.1, 101.9/160.5, 104.7/149.6, 92.8/140.9, 14.7/23.5 Kops/s; [log](log/2026-09-05-rust-multi-tcp-before.ko.md) |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 보류(67.3%) | 보류(66.0%) | 보류(66.1%) | 보류(69.3%) | 보류(72.3%) | 미측정 | 판정 `보류`(pass 1·2 뒤 3-run 값은 log) — before; aggregate 68.2%(목표 85%), latency 1.54x; 처리량 Rust/C 97.4/144.8, 90.2/136.7, 89.2/135.0, 81.0/116.8, 15.9/22.1 Kops/s; [log](log/2026-09-05-rust-multi-tcp-before.ko.md) |
| `tcp` | `MULTI_PUBSUB` | 보류(67.7%) | 보류(71.1%) | 보류(84.6%) | 보류(87.0%) | 보류(105.4%) | 미측정 | 판정 `보류`(pass 1·2 뒤 3-run 값은 log) — before; aggregate 83.2%(목표 95%), latency 0.96x; 처리량 Rust/C 443.9/655.3, 588.6/828.3, 719.7/850.3, 584.1/671.2, 67.5/64.0 Kmsg/s; [log](log/2026-09-05-rust-multi-tcp-before.ko.md) |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |

### 9.7 Python

- perf 경로: `bindings/python/perf`
- Single 상태: `미측정`
- Multi 상태: `보류` — before 19:32: DD 6.6%, REQREP 14.2/15.4%, PUBSUB 26.2% → pass 1(`72d6f8ec8a`)·pass 2(`2ad52c4e11`: hot path C 확장, Python 함수 79→9) quiet 3-run 09-06 00:22: DD 15.4%(32.5k msg/s), DR/RR 15.5/19.6%, PUBSUB 31.9%(목표 60) — 두 pass 완료, GIL 상한으로 `보류` 확정(§7.5); [log](log/2026-09-05-python-multi-tcp-before.ko.md)
- 다음 작업: inventory gate에서 확인한 pattern으로 paired 측정을 시작한다.

#### 9.7.1 Single suite

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|-------|--------|--------|------------------|
| `tcp` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `tcp` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ws` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ws` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `wss` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `wss` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `tls` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `tls` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `inproc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `inproc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `inproc` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ipc` | `PAIR` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |
| `ipc` | `ROUTER_ROUTER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ipc` | `ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | Core 0.17.1 artifact로 재측정 대상 |

#### 9.7.2 Multi suite

| Transport | Pattern | 64 | 256 | 1024 | 4096 | 65536 | 131072 | 결과 파일 / 메모 |
|-----------|---------|----|-----|------|------|-------|--------|------------------|
| `tcp` | `MULTI_DEALER_DEALER` | 보류(1.6%) | 보류(1.7%) | 보류(2.2%) | 보류(4.8%) | 보류(22.6%) | 미측정 | 판정 `보류`(pass 1·2 뒤 3-run 값은 log) — before; aggregate 6.6%(목표 60%), 15k msg/s 고정(메시지당 ~65 µs Python 고정 비용); 처리량 Py/C 15.2/922.3, 15.4/889.2, 15.3/688.4, 15.2/319.2, 13.4/59.2 Kmsg/s; `p1python`; [log](log/2026-09-05-python-multi-tcp-before.ko.md) |
| `tcp` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_DEALER_ROUTER_REQREP` | 보류(7.8%) | 보류(7.6%) | 보류(8.2%) | 보류(9.6%) | 보류(37.9%) | 미측정 | 판정 `보류`(pass 1·2 뒤 3-run 값은 log) — before; aggregate 14.2%(목표 60%), latency 5.99x; 처리량 Py/C 13.2/169.4, 13.0/170.3, 12.9/157.8, 12.5/130.6, 9.4/24.8 Kops/s; [log](log/2026-09-05-python-multi-tcp-before.ko.md) |
| `tcp` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tcp` | `MULTI_ROUTER_ROUTER_REQREP` | 보류(8.7%) | 보류(8.7%) | 보류(8.9%) | 보류(9.9%) | 보류(40.7%) | 미측정 | 판정 `보류`(pass 1·2 뒤 3-run 값은 log) — before; aggregate 15.4%(목표 60%), latency 6.80x; 처리량 Py/C 11.9/137.1, 11.8/136.1, 11.4/128.4, 11.3/113.7, 8.7/21.3 Kops/s; [log](log/2026-09-05-python-multi-tcp-before.ko.md) |
| `tcp` | `MULTI_PUBSUB` | 보류(21.7%) | 보류(17.2%) | 보류(13.7%) | 보류(18.0%) | 보류(60.5%) | 미측정 | 판정 `보류`(pass 1·2 뒤 3-run 값은 log) — before; aggregate 26.2%(목표 60%), latency 1.09x; 처리량 Py/C 141.1/651.6, 134.6/780.4, 123.9/903.0, 124.7/691.1, 43.4/71.6 Kmsg/s; [log](log/2026-09-05-python-multi-tcp-before.ko.md) |
| `tcp` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `ws` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `ws` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `wss` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `wss` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |
| `tls` | `MULTI_DEALER_DEALER` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_DEALER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_SENDSEND` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_ROUTER_ROUTER_REQREP` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_PUBSUB` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |  |
| `tls` | `MULTI_STREAM` | 미측정 | 미측정 | 미측정 | 해당 없음 | 미측정 | 해당 없음 |  |


## 10. 전체 진행 상태

### 10.1 사전 조건

| 구분 | 상태 | 결과 파일 / 메모 |
|------|------|------------------|
| Core REQUEST 계약 통일 | 완료 | `7d8205a028` |
| binding REQUEST 포팅 | 완료 | C, C++, .NET, Java, Node, Go, Rust, Python 8개 완료 |
| C perf REQREP runner | 완료 | `e5770ec569` 병합, 후속 계약 정렬 `0f9c329764` |
| 버전 3곳 일치 | 완료 | `VERSION`·`core/CMakeLists.txt:11`·`core/include/zlink.h:8-10` 모두 0.17.0 (2026-09-07 확인) |
| 실제 runtime 버전 | 완료 | `core/build/lib/libzlink.so.0.17.0`, Build ID `af759a1c5532fb7100c6baede89144814200d798`, SHA-256 `0af61ad3…`, commit `c39f50f6dc`, Release+LTO. C·C++ smoke report `META,core_version,0.17.0`·`core_dirty,0`, C++ `zlink::version()` 0.17.0 확인. **`--core-version 0.17.0` 금지** — `~/.cache/zlink/core/0.17.0/...`는 `core/build-dev` 유래 RelWithDebInfo/LTO OFF의 다른 artifact다 |
| runner inventory | **불일치 5건** | pattern 집합·transport·size·clients·io thread는 일치. 불일치: (1) C++ server DEALER effective HWM `4096000` vs C `1048576`, (2) monitor HWM 이름·단위·기본값(`monitor_hwm_bytes` bytes 4096000 ↔ `monitor_hwm` 1000), (3) `default_stream_clients` 100 ↔ 10000, (4) Effective Options key 이름(`routed_echo_per_socket_payload` ↔ `routed_echo_borrow_payload` 등), (5) `select_transports()`의 `CONTROL_PLANE_PATTERNS` 처리·순서 규칙. §4에 따라 정리 전에는 paired 측정을 시작하지 않는다 |
| Multi size 정책 | 완료 | C 기본 64, 256, 1024, 4096, 65536, 131072; STREAM 64, 256, 1024, 65536으로 §3과 일치 |
| 무시되는 runner option | 미확인 |  |
| memory guard | 미확인 |  |
| 재현 환경 manifest | 완료 | `log/2026-09-07-environment.ko.md` (현재 호스트 20 논리 CPU / 94 GiB 실측; 2026-09-05 manifest는 이전 호스트 기록) |
| Core artifact 고정 | 임시 | `scripts/build-core.sh release`(Release+LTO, `ZLINK_BUILD_TESTS=OFF`), runner는 `ZLINK_CORE_SOURCE=local`에서 `core/build`를 직접 사용 |
| smoke (C·C++ multi) | 완료 | `perf_c_multi_linux_20260907_080912.txt`·`perf_cpp_multi_linux_20260907_081121.txt` 모두 `status: complete`, 조건 완화 없음 |
| 러너 측정 의미 정합 | 진행 중 | 설계 `log/2026-09-07-runner-parity-design.ko.md`. C canonical 러너와 7개 binding 러너의 모든 pattern 측정 의미를 일치시킨 뒤 측정 시작(D-BP1) |

### 10.2 러너 정합 (측정 전 선결)

2026-09-07 러너 정합 설계(`log/2026-09-07-runner-parity-design.ko.md`)가 C와 7개 binding
러너의 정책 위반 37건과 정책 자체의 결함 6건을 정리했다. 정책 만족이 러너의 유일한 합격
기준이므로(D-BP2) 아래 단계를 끝낸 뒤 측정을 시작한다.

| 단계 | 내용 | 상태 |
|------|------|------|
| 0 | 감독자 결정 확보 — Single REQREP 제외(D-BP3), Single gate 제외(D-BP2), Go D-B123 대체(D-BP3) | 완료 |
| 1 | 측정 조건 통일 — server auto-HWM 보고 시점, monitor HWM report key, `default_stream_clients`, Effective Options key, `select_transports()`, C++ stale 검사, Go auto-HWM recalc 누락, .NET `--runs`·`--pin-cpu` 전달 누락 | 완료 `87153dd4f3` — C↔C++ Effective Options 완전 일치 검증 |
| 2 | 정책 문서 개정 — Single에서 REQREP 제외, 시간원 monotonic 고정(`sent_ts_ns` 표기 정정), active 유효 메시지 규칙, 가중 분위수 보간, 1ms 재시도 예외 | 완료 `506086e7cd` |
| 3 | C 러너를 정책에 맞춤 — 별도 latency 단계 제거, one-way 하드코딩 `max_in_flight=1` 제거, active deadline 필터, wire 길이 집계 제외, 가중 분위수 보간, matched client 빌드 파손 수정. REQREP은 D-BP6으로 복원·정합 | 완료 `522df6d57e`·`074d2a5964` |
| 4 | Multi 종료 protocol 정합 — `CLIENT_DONE`/`STOP` 대기 없이 socket을 닫는 6개 binding, Java의 wire stop token, Node의 비표준 barrier, Rust·Python PUBSUB의 `CLIENT_DONE` 누락 | 완료 `d634417a37` (7개 binding) |
| 5 | Multi 부하 수준 정합 — socket당 in-flight 1인 러너를 비동기 연속 제출로 | 부분 완료 `d634417a37` — C++·Java REQREP과 Python SENDSEND 전환, 미완료 상한 `PERF_MULTI_REQREP_MAX_OUTSTANDING`=64. 잔여: Go REQREP 재구성, .NET·Node·Rust·Python 상한 적용 |
| 6 | Single 러너 정책 정합 — 전용 OS thread + synchronous API, 언어별 금지 조항, REQREP 연속 제출 | C·C++·.NET·Java·Node 완료. Go·Rust·Python 진행 중(D-BP11) |
| 6b | 시간원 monotonic + 메시지당 `getenv` 캐시 + 미완료 상한 + 종료 protocol | **완료** — 7개 binding 전부(`b93b176061` Rust·Python, `fdfd4dda0b` Go). Go REQREP만 Core 결함으로 보류(D-BP12) |
| 6c | 잔여 정합 — `reqrep_max_outstanding` 8개 러너 노출, .NET `completed` anchor 위치, one-way 5건(active deadline 필터·wire 길이 검증·1 ms transient 재시도·RESULT 정밀도·latency 표본 0개) 7개 binding 이식, anchor 6종 대조표 | **완료** `af431a08c6` — 8개 러너 × 6 anchor × single/multi 대조 완료, 어긋난 항목 없음 |
| 6d | **C 기준 모델 복원** — 미완료 상한 제거(정책 `PERF_POLICY.md:271-274` 위반 시정), C++·Java multi REQREP을 turn당 socket 1건 구조로, poller 등록을 역할별로(requester `POLLCOMPLETION` 단독 / replier·one-way `POLLIN` / `POLLOUT` 없음), 러너의 backpressure 흉내 코드 제거 | 진행 중 |
| 7 | Core 0.17.1 artifact 고정 + 환경 manifest | 완료 — **저장소 밖 고정 prefix** `~/.cache/zlink/core-pinned/0.17.1`(태그 `core/v0.17.1` 커밋 `4cd03b9173`, Build ID `101bdb24…`). 모든 러너는 `ZLINK_CORE_SOURCE=release` + `ZLINK_CORE_PACKAGE_PREFIX`로 실행한다. 머신 B의 Core 변경과 분리(D-BP9) |
| 8 | 측정 — C++부터 pattern·transport 단위 paired | 진행 중 — `tcp` `MULTI_DEALER_DEALER` **통과(95.59%)** |

### 10.3 언어 진행 상태

**측정은 새로 한다.** 호스트와 Core artifact가 바뀌었으므로 모든 성능 수치는 **고정 Core
prefix** `~/.cache/zlink/core-pinned/0.17.2/lib/libzlink.so.0.17.2`(Build ID
`c5dec25e86de575e8efa79c5327b1e3d0d424f3c`, SHA-256 `72d508f73a5d261f…`, 태그 `core/v0.17.2`
커밋 `dca377aa5e`)에서 C와 새로 짝지어 다시 잰다.

> **2026-09-08 04:50 재고정 (D-B216 요청).** 0.17.2 릴리스로 bindings 버전이 올라가
> `Core release prefix version 0.17.1 does not match 0.17.2`로 측정이 거부되므로 재고정은
> 선택이 아니라 전제다. **0.17.1로 얻은 모든 판정은 참고값으로 강등하고 0.17.2에서 다시
> 잰다**(D-BP19와 같은 규칙). 0.17.1에서 만든 **작업 자산은 유효하다** — 러너 정합 수정 4건
> (러너 C 모델 복원 `31c5e4f7f0`, relay 직렬화 `e0862e1e5c`, client echo drain `33f63ae89d`,
> C `ctx_term` `1aa2751b1b`), REQREP 비용 지도(D-BP26), 결함 진단과 보고서.
>
> **재고정 절차**(다음에 또 필요하다): (1) 태그에서 worktree를 만들어
> `scripts/build-core.sh release`; (2) 산출물을 저장소 밖 prefix에 설치하고
> `share/zlink/core-package-provenance.json`을 남긴다; (3) **각 언어마다 `--reuse-build` 없이
> 전 pattern을 한 번 빌드한다** — 러너가 `bindings/<lang>/build-release-<version>/`을 쓰므로
> pattern 하나만 지정해 빌드하면 나머지가 없어 이후 실행이
> `benchmark runtime check target is missing`으로 실패한다.
`ZLINK_CORE_SOURCE=release`와 `ZLINK_CORE_PACKAGE_PREFIX`로 이 prefix를 가리킨다 — 머신 B가
`core/`에 계속 커밋하므로 작업 트리 build를 기준으로 삼으면 staleness 검사가 깨진다.
2026-09-05/06의 수치는 아래 표에 **참고**로만 남긴다.

유지하는 것은 수치가 아니라 **작업 자산**이다. 7개 언어에 적용·푸시된 개선 pass 15건의 코드,
callgrind·프로파일 분석, 후보 no-go 목록과 그 근거(D-B121~D-B130), 러너 결함 수정은 그대로
유효하며 되돌리지 않는다. 새 측정은 그 코드 위에서 시작한다.

| 순서 | 언어 | 현재 artifact Single | 현재 artifact Multi | 이전 호스트 Multi `tcp` 참고값 (DD / DR REQREP / RR REQREP / PUBSUB) | 적용된 개선 pass |
|------|------|------|------|------|------|
| 1 | C++ | REQREP 연속 제출 정합 완료(63.6%) | `tcp` DD **통과(95.42%)**, PUBSUB **통과(95.52%)**, DR REQREP **미달(72.87%)**, RR REQREP **미달(76.39%)** — SENDSEND 2종·STREAM 미측정, `tls`·`ws`·`wss` 미측정 | 90.8 / 57.4 / 68.4 / 93.2% | 3건 push + 러너 C 모델 정합 `31c5e4f7f0` |
| 2 | .NET | 미측정 | 미측정 | 59.6 / 58.3 / 67.0 / 61.3% | 3건 push |
| 3 | Java | 미측정 | 미측정 | 80.9 / 59.4 / 58.7 / 80.9% | 3건 push |
| 4 | Node | 미측정 | 미측정 | 35.9 / 24.3 / 24.8 / 30.2% | 4건 push |
| 5 | Go | 미측정 | 미측정 | 53.2 / 19.0 / 21.8 / 54.6% | 2건 push |
| 6 | Rust | 미측정 | 미측정 | 59.3 / 67.3 / 69.8 / 87.7% | 2건 push |
| 7 | Python | 미측정 | 미측정 | 15.4 / 15.5 / 19.6 / 31.9% | 2건 push |

참고값은 2026-09-06 00:22, 이전 호스트(16 논리 CPU / 11.7 GiB), Core `a40cb46335` 기준이다
(`doc/plan/c016-worklog/morning-summary-2026-09-05-B.ko.md`, D-B121~D-B130). 새 측정의 목표
달성 여부는 이 값이 아니라 새 paired 결과로 판정한다. 참고값과 새 값이 크게 다르면 Core
변경(223 파일)이나 호스트 차이의 영향이므로 원인을 log에 기록한다.

Single 상세 표(§9.x.1)의 기존 값도 같은 성격의 참고값이다. `PERF_POLICY.md:144-149`에 따라
Single은 cross-binding 성능 gate로 쓰지 않으며, 정책 준수와 lifecycle 검증을 위해 새로 측정한다.

작업 순서는 러너 정합(§10.2) 완료 후 §7.4대로 C++ Multi `tcp`부터 시작한다.

#### 10.3.1 남은 범위와 진행 순서 (2026-09-08 기준)

판정 단위는 pattern×transport다. 7 언어 × 4 transport × 7 pattern = **196 cell**이며, D-BP19에 따라
2026-09-05/06 값은 전부 참고값이므로 실질적으로 **약 190 cell이 미측정**이다. 지금까지 닫은 것은
C++ `tcp` 4개(`MULTI_DEALER_DEALER` 통과 94.73%, `MULTI_PUBSUB` 통과 95.16%,
REQREP 2종 보류 72.87%·76.39%)다.

측정 비용은 실측으로 paired 5-run이 cell당 약 5분, 탐색 1-run이 약 1분이다. 여기에 목표에서 먼
cell마다 개선 pass가 30~60분씩 붙는다. **§12를 문자 그대로 완료하는 것은 한 세션이 아니라 며칠
단위 작업이다.** 이 사실을 숨기지 않고 아래 순서로 가치가 높은 것부터 닫는다.

1. **C++를 끝까지 닫는다.** 남은 것은 `tcp` SENDSEND 2종(개선 pass 1 진행 중), `MULTI_STREAM`
   (C++·.NET만 smoke가 client-ready 경계로 실패해 조사 선행), `tls`·`ws`·`wss` 21 cell.
   **REQREP 4 cell(`tls`·`ws`·`wss` × 2)은 개선 pass를 새로 열지 않는다** — D-BP20이 확정한
   구조적 결론(Core 계약이 정한 drain-then-resubmit pacing과 그 위의 요청당 고정 비용)은
   transport를 모르므로, 측정 뒤 그 근거로 `보류`를 기록한다. §7.5가 2026-09-05에 같은 판단을
   내렸던 것과 같은 형태다.
2. **D-BP8 우선순위대로 .NET → Java → Node.** 각 언어는 `tcp` 7 pattern부터 닫고 transport를 넓힌다.
3. **Go는 0.17.2 대기**(D-BP12·D-BP14·D-BP16; 머신 B가 D-B211에서 차단 6건으로 MP-8 진행 중),
   **Rust·Python은 마지막**.

각 cell의 판정과 근거는 §9.x.2 표와 §11 기록, `decisions.ko.md`에 남긴다. 세션이 바뀌어도 그
기록만으로 다음 cell부터 이어받을 수 있어야 한다.

## 11. 측정 기록과 결과

paired 측정을 완료할 때마다 아래 표에 측정 조건과 결과만 한 행으로 추가한다. 실행 과정,
후보 검토, 프로파일과 구현 변경은 이 문서가 있는 폴더의 `log/`에 별도로 기록한다.

| 날짜 | 언어 | suite / 범위 | pair tag | 측정 조건 | 결과 | report |
|------|------|---------------|----------|----------------|------|---------------|
| 2026-09-05 | 전체 | 계획 초기화 | - | Core 0.17.0 local release build(LTO) `libzlink.so.0.17.0`, 정책 §1.2·§5.1 단일 phase runner, C 기준과 binding paired 비교, 단일 perf process 조건. 시작 조건: REQUEST 계약 통일(D-B85)과 binding REQUEST 포팅 반영 뒤. | 계획 작성 | 이 문서 |
| 2026-09-05 | 전체 | inventory gate | - | 7개 binding의 source pattern registry·CLI parser·README와 공식 single/multi wrapper `--help`를 대조했다. 모든 binding의 canonical 등록 목록은 Single 7개와 Multi 7개로 C runner와 같고, C 기본 크기와 STREAM 예외는 §3과 일치한다. 제외할 미등록 pattern과 C 크기 정책 불일치는 없다. 일부 README의 누락은 manifest에 기록했다. | 완료 | `log/2026-09-05-environment.ko.md` |
| 2026-09-05 | C++ | Multi `tcp` `MULTI_DEALER_DEALER`, `MULTI_DEALER_ROUTER_REQREP`, `MULTI_ROUTER_ROUTER_REQREP`, `MULTI_PUBSUB` before | `p1cpp` | 5 sizes(64~65536, `131072` 제외), 5초, 1회, 100 clients, I/O 4/4, auto-HWM balanced, Core 0.17.0 local Release+LTO `libzlink.so.0.17.0`(`053a568ddd`, clean), C 직후 C++ 순차 실행, 03:49~03:54 KST, load average 2.7~6.2는 실행 자체 부하 | before: 처리량 평균 75.0% / 51.8% / 60.7% / 93.3%로 모두 미달(목표 95/85/85/95%), latency 평균 0.58x / 0.80x / 0.77x / 1.06x로 통과; `MsgUnit(B)`는 양쪽 `?`; 개선 pass 전 | C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_{034919,035123,035218,035313}_p1cpp.txt`; C++: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260905_{035055,035151,035246,035341}_p1cpp.txt`; [log](log/2026-09-05-cpp-multi-tcp-before.ko.md) |
| 2026-09-05 | C++ | Multi `tcp` `MULTI_DEALER_DEALER`, `MULTI_DEALER_ROUTER_REQREP`, `MULTI_ROUTER_ROUTER_REQREP` 자체 hot-path 개선 pass 1 after | `p1cpp` C 기준(after report는 tag 없음) | before와 같은 5 sizes, 5초, 1회, 100 clients, I/O 4/4, auto-HWM balanced, 같은 Core artifact(`053a568ddd`, `core_dirty=0`)와 host session, 3 pattern을 한 report에 순차 실행, 04:13 KST, load average 0.77 0.55 1.08; 변경은 `bindings/cpp/**` 6개 파일(result/entry bundle 할당, map-node PMR pool, SEND submit-before-register, async terminal lock-free publish), 공개 헤더 diff 0줄 | after: 처리량 평균 75.0→90.8% / 51.8→55.8% / 60.7→68.4%로 모두 미달(목표 95/85/85%), latency 평균 1.52x / 0.75x / 0.67x로 통과(`DEALER_DEALER` 1024B 4.93x outlier); callgrind 1024B `new`/msg DD 3.42→1.39(C 0.26), REQREP 10.44/op(C 1.13); `DEALER_ROUTER_REQREP` 4096B −2.2%(단일 run); gate 전체 PASS(contract 16/16, smoke 7/7, stress PASS); Sol 리뷰 pass 전 | C: before와 동일 `perf_c_multi_linux_20260905_{034919,035123,035218}_p1cpp.txt`; C++ after: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260905_041338.txt`(작업 worktree 사본 `/home/hep7hep7/project/zlink-wt-cpp-perf/bindings/cpp/perf/results/multi/report/`); [log](log/2026-09-05-cpp-multi-tcp-pass1.ko.md) |
| 2026-09-05 | C++ | Multi `tcp` `MULTI_DEALER_DEALER`, `MULTI_DEALER_ROUTER_REQREP`, `MULTI_ROUTER_ROUTER_REQREP` Sol read-only 리뷰 기반 개선 pass 2 after, transport 판정 확정 | `p1cpp` C 기준(after report는 tag 없음) | pass 1과 같은 5 sizes, 5초, 1회, 100 clients, I/O 4/4, auto-HWM balanced, 같은 Core artifact(`ee50ebaeaf`, `053a568ddd` 대비 `core/` diff 없음, `core_dirty=0`)와 host session, 3 pattern을 한 report에 순차 실행, 04:35 KST, load average 0.28 0.53 0.83; Sol 리뷰 후보 9개 중 계약 보존 3개 채택(bundle 내부 resume slot, reply `message_t` 직접 adopt, socket당 inline 첫 completion entry), 6개 no-go(public ABI·ABA·중복·측정 의미); 변경은 `bindings/cpp/src/Runtime/Messaging/**` 5개 파일, 공개 헤더 diff 0줄 | after: 처리량 평균 90.8→90.8% / 55.8→57.4% / 68.4→68.4%, latency 평균 0.62x / 0.72x / 0.68x로 통과; callgrind DR 1024B Ir/op 49.16k→48.57k, `new`/op 10.44→9.42, move/op 5→3; 단일 run 하락 `DEALER_ROUTER_REQREP` 64B −6.73%, `ROUTER_ROUTER_REQREP` 256B −13.08%는 재측정 없이 기록; gate 전체 PASS(contract 16/16, smoke 7/7, 관련 4종 각 5회, stress PASS); 판정: `DEALER_DEALER` `통과(90.8%)`(§2.1 완화 목표 90% 선택, 개별 최소 미달 64/256/1024 outlier), `DEALER_ROUTER_REQREP` `보류(57.4%)`, `ROUTER_ROUTER_REQREP` `보류(68.4%)`(§7.4 16단계: 두 pass 뒤 public contract 유지 후보 없음), `MULTI_PUBSUB` `미달(93.3%)` 유지(기본 목표 95%, 개선 pass 전) | C: before와 동일 `perf_c_multi_linux_20260905_{034919,035123,035218}_p1cpp.txt`; C++ pass 2 after: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260905_043517.txt`(작업 worktree 사본 `/home/hep7hep7/project/zlink-wt-cpp-perf2/bindings/cpp/perf/results/multi/report/`); callgrind `perf_cpp_multi_linux_20260905_043356_cpp_pass2_callgrind_rr.txt`; [log](log/2026-09-05-cpp-multi-tcp-pass2.ko.md) |
| 2026-09-05 | C++ | Multi `tls`·`ws`·`wss` `MULTI_DEALER_DEALER`, `MULTI_DEALER_ROUTER_REQREP`, `MULTI_ROUTER_ROUTER_REQREP`, `MULTI_PUBSUB` before | `p1cpp-tls`, `p1cpp-ws`, `p1cpp-wss` | 5 sizes(64~65536, `131072` 제외), 5초, 1회, 100 clients, I/O 4/4, auto-HWM balanced, Core 0.17.0 local Release+LTO `libzlink.so.0.17.0`(`tcp`와 같은 artifact, `core_dirty=0`; `META,core_revision` `tls`·`ws` `6e8d798bac`, `wss` `296c5c04e5`는 문서 2개만 차이), C++는 `tcp` pass 2 코드(`e6dd88fbc6` 포함), transport마다 4 pattern을 C 직후 C++ 순차 실행, 04:41~04:55 KST, load average 0.7~8.5는 24회 연속 실행 자체 부하 | before(판정 미확정): `DEALER_DEALER` 처리량 평균 77.6% / 84.9% / 93.9% 미달(목표 95%), latency 6.23x(`tls`, 상한 초과; 1024B 25.96x·4096B 3.97x outlier) / 0.63x / 1.91x(1024B 5.60x·4096B 2.19x outlier); `DEALER_ROUTER_REQREP` `tls` 54.5% 미달(0.43x); `ROUTER_ROUTER_REQREP` `tls` 60.5%·`ws` 58.0% 미달(0.38x / 0.59x); `PUBSUB` 100.2% / 104.1% / 104.8% 통과 후보(0.97x / 1.09x / 0.92x; 64B 80.9~85.7%), §7.4 14단계 검토 전; C 기준 이상: C runner `ws` `DEALER_ROUTER_REQREP` 4096B 7,867.2 ops/s / 58.6 ms(1024B 127,386.8 / 1.98 ms), `wss` `DEALER_ROUTER_REQREP` 4096B 3,413.4 / 22.4 ms, `wss` `ROUTER_ROUTER_REQREP` 4096B 16,836.6 / 20.4 ms — C++ 같은 셀 51,107.0 / 32,961.4 / 35,799.0 ops/s 정상 — 해당 셀 `보류(C 기준 이상)`, 세 transport aggregate 미확정(4 size 참고 55.1% / 57.6% / 55.4%), Core/runner 조사 job 개설(brief `core-c-ws-reqrep-4k`), Core 재빌드·수정 없음(§5); `MsgUnit(B)`는 양쪽 `?`, memory guard cap 없음, 실제 clients 100 | C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_{044211,044417,044513,044610}_p1cpp-tls.txt`, `{044714,044810,044906,045001}_p1cpp-ws.txt`, `{045104,045202,045258,045355}_p1cpp-wss.txt`; C++: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260905_{044349,044446,044542,044638}_p1cpp-tls.txt`, `{044743,044839,044934,045029}_p1cpp-ws.txt`, `{045133,045231,045327,045424}_p1cpp-wss.txt`; [log](log/2026-09-05-cpp-multi-tls-ws-wss-before.ko.md) |
| 2026-09-05 | C++ | Multi `ws`·`wss` `MULTI_DEALER_ROUTER_REQREP`, `MULTI_ROUTER_ROUTER_REQREP` before 재측정(C runner 수정 D-B89 뒤) | `p1cpp-ws-fix`, `p1cpp-wss-fix` | 5 sizes, 5초, C 5회 평균·C++ 1회, 100 clients, Core 0.17.0 local(`core/build`), C runner WS/WSS byte-quantum 턴, C++ pass 2 코드, 05:27~05:33 KST, load 0.3~1.4에서 시작 | before(판정 미확정): `DEALER_ROUTER_REQREP` `ws` 53.1%·latency 2.43x, `wss` 43.7%·1.02x; `ROUTER_ROUTER_REQREP` `ws` 72.3%·2.54x, `wss` 44.7%·0.75x — 모두 `미달`; `보류(C 기준 이상)` 3셀 해소; C++ `ws` 64~1024B가 before보다 45% 높아 run-to-run 편차 주의 | [log](log/2026-09-05-cpp-multi-ws-wss-reqrep-remeasure.ko.md) |
| 2026-09-05 | C++ | Multi `tcp` `MULTI_PUBSUB` 자체 hot-path pass 1 + 3-run 재짝지음 | `p1cpp-pubsub-r3` | 5 sizes, 5초, C·C++ 각 3회 평균, 100 clients, Core 0.17.0 local(`3480ee5d78`), C++ pass 2 코드(변경 없음), 05:53~05:57 KST, load 0.18에서 시작 | pass 1 no-go(subscriber wrapper 약 5%, 계약 유지 후보 없음); 3-run aggregate 81.5%·latency 1.08x `미달`(목표 95%) | [log](log/2026-09-05-cpp-multi-tcp-pubsub-pass1.ko.md) |
| 2026-09-07 | C++ | Multi `tcp` `MULTI_DEALER_DEALER` before | `p2cpp` | 5 sizes(64~65536), 5초, 1회, 100 clients, I/O 4/4, auto-HWM balanced, **Core 0.17.1** local Release+LTO `libzlink.so.0.17.1`(Build ID `f7e2a539…`, SHA-256 `79cc4358…`, source `074d2a5964`), C 직후 C++ 순차 실행, 10:42~10:43 KST, load average 0.96 | before: 처리량 68.5 / 70.5 / 93.3 / 96.6 / **54.9**% → aggregate **76.8%** `미달`(목표 95%, 완화 90%); latency 1.03 / 0.14 / 0.28 / 1.12 / 2.06x → aggregate 0.93x `통과`(상한 2.0x); C ops/s 1,682,102 / 1,533,516 / 1,224,382 / 640,748 / 161,487, C++ 1,151,636 / 1,080,648 / 1,142,451 / 618,912 / 88,712; 병목 두 갈래 — 작은 크기(64·256) 68~70%는 메시지당 고정 비용, 65536B 54.9%는 대형 payload 경로; 중간 크기(1024·4096) 93~97%는 거의 정합. 개선 pass 전 | C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260907_104232_p2cpp.txt`; C++: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_104306_p2cpp.txt`; 환경 [manifest](log/2026-09-07-environment-0.17.1.ko.md) |

| 2026-09-07 | C++ | Multi `tcp` `MULTI_DEALER_DEALER` pass 1 after, 고정 artifact 재짝지음 | `p3pin` | 5 sizes(64~65536), 5초, 1회, 100 clients, I/O 4/4, auto-HWM balanced, **고정 Core 0.17.1 prefix** `~/.cache/zlink/core-pinned/0.17.1/lib/libzlink.so.0.17.1`(Build ID `101bdb24…`, SHA-256 `a3e00fd2…`, 태그 `core/v0.17.1` 커밋 `4cd03b9173`, `core_dirty=0`), `ZLINK_CORE_SOURCE=release`, C 직후 C++ 순차, 12:26~12:28 KST, load 0.00 | after: 83.4 / 85.1 / 99.8 / 100.6 / 109.1% → aggregate **95.59%** `통과`(기본 목표 95% 충족, 완화 목표 불필요); latency 1.20 / 0.24 / 0.79 / 1.04 / 0.97x → aggregate 0.85x `통과`; C ops/s 1,690,627 / 1,541,361 / 1,263,339 / 669,816 / 167,148, C++ 1,410,626 / 1,311,114 / 1,260,384 / 673,771 / 182,319; 개별 최소 85% 미달은 64B(83.4%) 하나로 outlier 기록; 구 artifact 측정(93.74%)보다 소폭 상승 | C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260907_122627_p3pin.txt`; C++: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_*_p3pin.txt`; [pass 1 log](log/2026-09-07-cpp-multi-tcp-dd-pass1.ko.md) |

| 2026-09-07 | 7개 binding | Single `DEALER_ROUTER_REQREP` `tcp` 64B — 러너 정책 정합 후 자릿수 확인 | 정합 확인용 | 64B, 1초, 1회, 고정 Core 0.17.1 prefix(태그 `core/v0.17.1`), 각 러너를 직렬 실행, `PERF_FAIL_FAST=1` | C 841,588 ops/s 대비 — C++ 534,982(63.6%), Rust 357,655(42.5%), .NET 313,245(37.2%), Java 259,238(30.8%), Node 149,844(17.8%), Python 17,917(2.1%), **Go 측정 불가**(하위 계약 결함). 정합 전 전 언어가 in-flight 1로 수천 ops/s대였다(.NET 3,542 = C의 0.46%). Python은 Node 대비 8.4배 낮아 별도 진단 대상, Go는 같은 socket에 동시 multipart request가 `EINVAL`/`EAGAIN`으로 실패해 별도 조사 대상. 이 값들은 정합 여부를 가리는 자릿수 확인이며 판정용 paired 측정이 아니다 | [러너 정합 log](log/2026-09-07-priority-runner-parity.ko.md), [Go·Rust·Python](log/2026-09-07-go-rust-python-runner-parity.ko.md), [.NET 진단](log/2026-09-07-dotnet-single-reqrep-diagnosis.ko.md) |

| 2026-09-07 | C++ | Multi `tcp` 4 pattern before — 러너 C 모델 정합 후 | `p5cmodel` | 5 sizes(64~65536), 5초, 1회, 100 clients, I/O 4/4, auto-HWM balanced, 고정 Core 0.17.1 prefix(태그 `core/v0.17.1` `4cd03b9173`, `core_dirty=0`), 러너는 상한 제거·turn당 socket 1건·requester `POLLCOMPLETION` 단독 정합 후(`31c5e4f7f0`), C 직후 C++ 순차, 22:08~22:2x KST, load 0.15에서 시작 | `MULTI_DEALER_DEALER` **통과(95.42%)** — 81.3/87.5/100.8/98.8/108.6%, latency 0.83x; `MULTI_PUBSUB` **통과(95.52%)** — 89.8/89.6/94.9/97.9/105.4%, latency 1.01x; `MULTI_DEALER_ROUTER_REQREP` **미달(72.87%)** — 75.1/70.2/72.3/73.9/73.0%, latency 2.69x(65536B 9.68x); `MULTI_ROUTER_ROUTER_REQREP` **미달(76.39%)** — 75.9/74.8/74.9/76.5/79.8%, latency 2.15x(65536B 6.36x). REQREP은 크기와 무관하게 70~80%로 평평해 메시지당 고정 비용이 아니라 요청 경로 전반의 오버헤드다. 65536B에서만 latency가 6~10배로 튀어 aggregate latency 상한 2.0x를 넘겼다 | C: `bindings/c/perf/results/multi/report/*_p5cmodel.txt`; C++: `bindings/cpp/perf/results/multi/report/*_p5cmodel.txt` |

| 2026-09-07 | C++ | Multi `tcp` `MULTI_DEALER_DEALER`·`MULTI_PUBSUB` — §7.2 '최종·경계 판정' 5-run 재측정 | `p6b5run` | 5 sizes(64~65536), 5초, **5회**, 100 clients, I/O 4/4, auto-HWM balanced, 고정 Core 0.17.1 prefix(태그 `core/v0.17.1` `4cd03b9173`, Build ID `101bdb24…`, `core_dirty=0`), `ZLINK_CORE_SOURCE=release`, 매 실행 전 `wait-for-idle-perf.sh`와 load>5 대기, C 직후 C++ 순차, 23:16~23:26 KST | `MULTI_DEALER_DEALER` **통과(94.73%)** — 82.1/81.1/101.8/99.5/109.1%, latency 0.745x; §2.1 완화 목표 90% 선택(pass 3회·후보 9개 기각), 개별 최소 미달 outlier 64 B·256 B. `MULTI_PUBSUB` **통과(95.16%)** — 88.9/94.2/90.9/111.5/90.4%, latency 1.007x; 기본 목표 95% 충족, 개별 최소 미달 없음. 두 셀의 1-run 값(95.42%·95.52%)은 §7.2 '탐색' 조건이라 판정으로 쓰지 않는다 — pass 2가 library source diff 0줄로 DD 1024 B 처리량 −5.52%·latency +25.00%를 재현해 1-run 변동폭이 목표 여유보다 큼을 보였다(D-BP21) | C `perf_c_multi_linux_20260907_{231628,232122}_p6b5run.txt`; C++ `perf_cpp_multi_linux_20260907_{231855,232356}_p6b5run.txt`; 네 report 모두 `status: complete`, `success: 5` |

| 2026-09-07 | C++ | Multi `tcp` `MULTI_DEALER_ROUTER_SENDSEND`·`MULTI_ROUTER_ROUTER_SENDSEND` before | `p7explore`(1-run 탐색) → `p8ss5r`(5-run 판정) | 5 sizes(64~65536), 5초, 탐색 1회 후 판정 **5회**, 100 clients, I/O 4/4, auto-HWM balanced, 고정 Core 0.17.1 prefix(`core/v0.17.1` `4cd03b9173`, `core_dirty=0`), 매 실행 전 idle·load>5 대기, C 직후 C++ 순차, 23:27~23:39 KST | DR SENDSEND **미달(92.37%)** — 91.1/94.7/90.4/90.4/95.3%, latency 1.134x 통과. RR SENDSEND **미달(92.45%)** — 88.5/90.7/86.6/93.2/103.3%, latency 1.291x 통과. 둘 다 기본 목표 95% 미달·완화 목표 90% 초과이며, 개선 pass 전이라 완화 목표를 적용하지 않는다. **1-run 탐색이 걸러진 사례**: RR 4096 B에서 C가 282.9 Kmsg/s·337.24 ms(p99 997 ms)로 무너져 aggregate가 98.11%로 부풀었으나 5-run에서 392.6 Kmsg/s·0.284 ms로 재현되지 않았다. 1회성 outlier로 판정에서 제외했다(D-BP21) | C `perf_c_multi_linux_20260907_{232939,233439}_p8ss5r.txt`; C++ `perf_cpp_multi_linux_20260907_{233213,233704}_p8ss5r.txt`; 네 report 모두 `status: complete`, `success: 5` |

| 2026-09-08 | C++ | Multi `tcp` SENDSEND 2종 개선 pass 1과 판정 | `p8ss5r` before / 확인 재측정 | before와 같은 5 sizes·5초·**5회**·100 clients·고정 Core 0.17.1 prefix. pass는 library를 수정하지 않았고 확인 재측정은 **변경 없는 source**다(최적화 after가 아니다) | **후보 없음** — routed send 고유 비용에 격차가 없다: native routed send inclusive C 5,278 / C++ 5,278 Ir, RR client native는 C 5,627 / C++ 5,362로 C++가 낮고, C++ builder 추가분은 132 Ir뿐. 축소 가능 항목 3개 합계 약 250 Ir는 64 B application 구간 차이 1,732 Ir의 14%로 전부 구현해도 약 93.5%(감독자 확인, pass 2 미개설). 판정 **DR `통과(92.37%)`·RR `통과(92.45%)`**(§2.1 완화 목표 90%). 기능 27/27, 회귀 gate 5-run 통과(처리량 최대 −2.30%, latency 최대 +5.08%), 공개 헤더 diff 0줄. **5-run 재현성**: source 변경 0줄 재측정에서 DR −0.13%p, RR −0.21%p(1-run은 ±5%p) | [pass 1 기록](log/2026-09-08-cpp-multi-sendsend-pass1.ko.md) |


## 12. 완료 기준

다음 조건을 모두 만족해야 작업을 완료한다.

- runner, 정책, 상세 표의 pattern, transport, size inventory가 일치한다.
- 각 pattern의 최종 판정에 사용한 고정 Core **0.17.2** prefix 기준 C와 binding paired report가
  모두 `status: complete`다. 0.17.1 기준 값과 짝짓지 않는다(D-B216).
- 모든 binding의 **Multi** 상세 표에 `미측정` 또는 `미달`이 없다(D-BP2: Single은 성능
  판정 대상이 아니다).
- 모든 binding의 **Single** 러너가 `PERF_SINGLE_TEST_POLICY.md` §1.1의 실행 모델(전용 OS
  thread + synchronous API, 측정 구간 coroutine/async task/Promise·Future executor/event-loop
  yield 금지, REQREP은 public request API가 허용하는 만큼 연속 제출 + reply completion drain)을
  만족하고, Single paired report가 `status: complete`로 기록되어 있다.
- C와 7개 binding의 러너가 `doc/perf`의 정책 문서(`PERF_POLICY.md`,
  `PERF_SINGLE_TEST_POLICY.md`, `PERF_MULTI_TEST_POLICY.md`)를 만족한다(D-BP2: 러너의 유일한
  합격 기준). 정책 요구사항 추적표에 위반 칸이 없다.
- 모든 통과 셀에 paired C와 binding report, manifest, 반복값, 비율, 옵션 일치 근거가
  기록되어 있다.
- throughput, 평균 latency, client 수, auto-HWM, 대상 외 대표 셀 회귀 gate를 측정값으로
  판정하고 원시 반복값을 기록했다.
- 변경한 binding의 단위 테스트와 통합 테스트가 통과한다.
- 한 언어의 모든 pattern이 각각 완료되기 전에는 다음 언어로 이동하지 않는다.
- 채택한 성능 개선은 검증된 범위만 커밋하고 원격에 푸시했으며 commit id를 기록했다.
- perf 전용 우회, private API 접근, 무시되는 필수 option, timeout/sleep 증가가 남아
  있지 않다.
- 최종 리뷰에서 public interface가 더 복잡해지지 않았고 비용이 binding 내부에서
  줄었는지 확인했다.
