# 0.17.0 캠페인 — 머신 B 실행 계획 (DONTWAIT 계약 B, bindings, perf 판정·개선)

> 작성일: 2026-09-05
> 선행 계획: [`core-send-dontwait-completion-0.16.0-plan-b.ko.md`](core-send-dontwait-completion-0.16.0-plan-b.ko.md)
> 작업 기록: [`c016-worklog/decisions.ko.md`](c016-worklog/decisions.ko.md) (D-B71~D-B85), 브리프 `c016-worklog/briefs/`
> 인계 문서(머신 A용): [`c016-worklog/handoff-A-dontwait-backpressure.ko.md`](c016-worklog/handoff-A-dontwait-backpressure.ko.md)
> bindings 성능 계획: [`../perf/perf/bindings-0.17.0/bindings-library-performance-improvement-plan-core-0.17.0.ko.md`](../perf/perf/bindings-0.17.0/bindings-library-performance-improvement-plan-core-0.17.0.ko.md)
> 최적화 가이드: [`../perf/BINDINGS_OPTIMIZATION_GUIDE.ko.md`](../perf/BINDINGS_OPTIMIZATION_GUIDE.ko.md)
> 역할: 머신 A = framework(필요 시 Core·bindings 버그 수정도 직접 커밋), 머신 B = Core·bindings·perf. 감독관(Claude) =
> 리뷰·판정·커밋, 구현 = codex sol high/medium·terra(ultra는 정말 필요할 때만, `service_tier=fast` 사용 안 함), 메모리
> 안전·필터에 걸리는 조사는 Claude 서브에이전트.

이 문서는 0.17.0 캠페인에서 머신 B가 맡은 일을 새 세션이 상위 기록을 다 읽지 않아도 이어갈 수 있게 정리한 실행
계획이다. 검증된 단위마다 `main`에 바로 커밋·push한다(A가 그 위에서 작업한다). 스펙(계약)을 새로 바꾸는 결정만 사용자가
내리고, 이미 결정된 계약의 스펙 문서 갱신은 감독관이 커밋한다.

## 0. 배경과 계약

- 0.16.0 pull-completion 전환이 DONTWAIT send를 "HWM에서 무제한 pending 수락"으로 바꿔 HWM이 흐름 제어 역할을 잃었고,
  multi 벤치가 정지·latency 폭증을 보였다(D-B71~D-B78). 사용자 결정으로 **계약 B**를 확정했다(D-B79):
  DONTWAIT FINAL은 admission 한 번 → 성공이면 ID 0·completion 없음, HWM/credit 부족·대상 미준비면
  `BACKPRESSURED`+`EAGAIN`+**대기 토큰**(payload는 호출자 보유) → credit 회복 시 `ZLINK_COMPLETION_WRITABLE`(토큰·context·RID)
  + `POLLOUT` level → 앱이 queue를 `NO_DATA`까지 pull하고 같은 record를 재제출. 토큰 종료: WRITABLE / 명시적 제거(TERMINAL+ENOENT) /
  close(TERMINAL). ROUTER route 없음(MANDATORY 양수)은 즉시 NOT_CONNECTED. PUB publish는 completion 비대상.
- **REQUEST도 같은 계약**으로 통일했다(D-B85, 2026-09-04 23:40): Core REQUEST pending pool 제거, `PENDING_MAX_*`는 ABI만 유지하는
  no-op. 근거는 perf 정책 §1.2("HWM은 send admission queue를 제한한다")와 64 KiB REQREP latency 60~75배(D-B84).
- 계약 원문: `core/doc/spec/core/socket/README.ko.md` "Part send" 절과 REQUEST 절, 헤더 주석 `core/include/zlink/socket/api.h`.
  버전은 Core·bindings 모두 0.17.0(`70a9998998`).

## 1. 완료된 것 (커밋 해시)

| 항목 | 커밋 |
|---|---|
| Core 계약 B(SEND) | `50d77800f2` |
| 스펙 14파일(ko/en) + 인계 문서 | `0377213d26`, `5e9d8f258e`(MANDATORY 정정) |
| 0.17.0 범프 | `70a9998998` |
| bindings 포팅 cpp/node/dotnet · java · c · rust · python · go | `4f503b76d3` · `7927c582c2` · `f9d0eb84d9` · `85eb9425a1` · `5240947587` · `4ff46b5bae` |
| bindings 리뷰(버그·핫패스·스모크) c · rust · dotnet · cpp · go · python · node · java | `fc0562cef4` · `9f2342cf27` · `8b52bb66ba` · `5a42a363c7` · `c6fdad2194` · `cd5b4a163e` · `a9eb6c5a77` · `b72622bb2d` |
| perf multi 러너 정책(§1.2·§5.1) 복원 | `00a668fe90` |
| Core DD 1024B latency 수정(토큰 등록 시 동기 mailbox drain 제거) | `89ed9be356` |
| 최적화 가이드, 0.17.0 bindings 성능 계획 | `5e9d8f258e`, `9248fb3728`, `ec4776e3c1` |

리뷰에서 잡은 큰 것: node 포팅 후 4배 회귀(28k → 145k msg/s), java 985 msg/s 붕괴(FFM scope close) → 789k, go 81k → 190k,
rust 실행기 spin 제거(172k → 259k), python 7.9k → 20k. 표는 인계 문서 §6과 가이드 §3.

## 2. Core 0.17.0 vs 0.15.1 판정 (정책 복원 러너, CCU=100, tcp, DUR=5)

판정 기준(사용자, 2026-09-04): **사이즈별 −5%까지만 허용, (패턴 × transport)의 사이즈 합계(gate geomean)는 조금이라도 떨어지면
개선 대상.** 측정은 조용한 머신에서 두 트리를 번갈아 runs=3 중앙값으로 확정한다(동시에 다른 perf가 돌면 오염됨, D-B82).

| 패턴 | 결과 | 남은 대상 |
|---|---|---|
| DEALER_DEALER | 64/256B +16/+3%, 1024B 동률, 4096B −8%(경계), 64K는 0.15.1 러너 timeout | 1024B latency mean +35%(수정 후, D-B83), 4096B 포화 구간 p95/p99 +30% |
| DR_SENDSEND | 64/256/4096/64K 이상, 1024B −10%(1회) → runs=3 +11% | 없음(재확인) |
| PUBSUB | 전 사이즈 +6~45% | 없음 |
| DR/RR_REQREP | 1024B 처리량 +15/+31%, **65536B −27%/−5%, latency 60~75배**, 64/256/4096B −6~−10%(1회) | REQUEST pending pool 제거(D-B85)로 해소 예상, 재측정 |

## 3. 진행 중 / 남은 작업 (순서)

1. **Core REQUEST 계약 B** — job 완료(dev 140/140, 새 public suite, pending pool 제거 29파일). main 포팅·gate·release lib 후 커밋·push,
   스펙(README REQUEST 절·PENDING_MAX·dealer/router, ko/en) 갱신 커밋. hotpath_gate는 release-gate 트리에서 별도 확인.
2. **bindings 8개 REQUEST 경로 포팅** — 4개씩 두 묶음(node·cpp·java·dotnet → c·go·rust·python), worktree + 메인 Core 빌드 symlink,
   브리프 `briefs/bindings-request-template.prompt`(SEND 토큰 기계 재사용, 즉시 성공 경로 무할당, TERMINAL typed 실패, 스모크 필수).
   각 묶음 green 시 커밋·push.
3. **C perf REQREP 러너**를 토큰 모델로(브리프 `briefs/perf-c-reqrep-token.b.prompt`), ci_multi_smoke·metrics test.
4. **재측정**: REQREP 2패턴 + DD·SENDSEND·PUBSUB, 0.15.1 대비 runs=3, D-B8x 기록. 남는 대상은 원인 하나 = job 하나.
5. **PUBSUB allocator abort**(`test_backpressure_oneway_matrix_pubsub_regression`, 1% 재현, D-B83): Claude 서브에이전트가 ASan으로
   원인·수정·회귀 테스트. codex는 sanitizer 출력에 콘텐츠 필터가 걸려 사용 불가.
6. **리팩토링 패스**(사용자 지시 2026-09-05 00:05): pending 잔여 코드·중복 정리(POSDDD), bindings 토큰 기계 공통화, 성능 상수 비용 —
   별도 커밋으로 push.
7. **bindings 성능 측정·개선**: 2번이 끝난 뒤 위 0.17.0 bindings 성능 계획대로 C 대비 비율을 pattern별로 측정·판정하고 가이드 §2
   체크리스트로 개선. 결과는 계획 문서 §9·§11에만 기록, 과정은 `log/`.
8. 남은 Core 대상(DD 4096B 포화 p95/p99, 1024B mean 잔여)은 auto-HWM 예산·I/O batching 과제로 사용자 결정 뒤 진행(D-B83).

## 4. 게이트와 규칙

- Core 커밋 전: `scripts/build-core.sh dev` + `ctest -j2` 전체, 변경 suite 5회, mirror cmp(8 헤더 × 4), `git diff --check`,
  release lib 재링크(`release --lib-only`), hotpath_gate(LTO 트리, ±5%). 메모리 11 GB: 빌드 JOBS≤6, 동시 job 4개 이하.
- bindings 커밋 전: 해당 `tests/run_tests.sh` + 샘플, perf single/multi 스모크(status complete, 0 없음), 새 테스트 5회.
- 측정 중 다른 빌드·벤치 금지(사용자 perf 포함). 측정 표에는 load average를 함께 남긴다.
- job 감시: 진행 파일 + **3분 간격 프로세스 생존 확인**(scope 상태만 보면 gradle 잔여 프로세스 때문에 오판).
- `pkill -f`는 자기 명령줄과 겹치는 패턴 금지(자기 자신을 죽인 사례 3회).

## 5. 체크리스트

- [x] Core 계약 B(SEND), 스펙, 범프, bindings 8 포팅, 리뷰 8, perf 러너 복원, DD latency 수정, 가이드·계획 문서
- [x] Core REQUEST 계약 B 커밋·push(`7d8205a028`), 스펙 갱신 커밋(`ea934d0e97`), hotpath_gate PASS(D-B86)
- [x] bindings REQUEST 포팅 1차 dotnet `2099bb045a`(병합)·java `a06260f507`·node `b145f86501`·cpp `e0860723bc` / 2차 python `78eed9ce96`·rust `9728a0e081`·go `7afa27e72b`·c `8decd5ed5c`(병합)
- [x] C perf REQREP 러너 토큰 모델(`e5770ec569` 병합)
- [x] 0.15.1 대비 재측정 표 — D-B87; RR_REQREP 65536B 원인·수정 `5e26e72806`(D-B88, −4.4% 허용). 남은 것은 latency 잔여(D-B83)
- [x] PUBSUB allocator abort 원인·수정 — SUB session use-after-free(ZMP decoder 지연 소멸), `29add0ac81`, 회귀 테스트 추가
- [x] 리팩토링 패스 커밋 — Core token-path 정리(dead code·책임 분리·이름·핫패스, hotpath ±0.04%); bindings 쪽 POSDDD 검토는 §6 언어별 pass(계획서 §7.4-14)에서 수행
- [ ] bindings 성능 측정·개선 — §6 실행 계획대로. 진행: C++ Multi tcp before/pass1/pass2 완료(DD 통과 90.8%, REQREP 보류 57/68%, PUBSUB 미달 93.3%; 커밋 `ee50ebaeaf`·`6e8d798bac`), tls/ws/wss before 측정 완료(기록 중), C 러너 ws/wss 4096B REQREP 붕괴 조사 job 진행 중; 다음 C++ PUBSUB pass → .NET
- [ ] 인계 문서 최종 갱신(REQUEST 계약·해시), 아침 요약

## 6. bindings 성능 계획(0.17.0) 실행 계획

[`bindings-library-performance-improvement-plan-core-0.17.0.ko.md`](../perf/perf/bindings-0.17.0/bindings-library-performance-improvement-plan-core-0.17.0.ko.md)
(이하 "계획서")를 §3의 2~5단계가 끝난 뒤 다음 순서로 실행한다. 계획서 §7.4의 20단계 작업 순서와 §7.5~§7.7 gate를 그대로 따르고,
이 절은 머신 B가 그것을 어떤 도구·job·커밋 단위로 수행하는지만 정한다.

### 6.1 시작 조건 (계획서 §1·§4·§6)

- main에 Core REQUEST 계약 B, bindings 8개의 REQUEST 포팅, C perf REQREP 러너가 모두 커밋돼 있고 §3-4 재측정 표(D-B8x)가 기록돼
  있어야 한다. 측정 runtime은 `core/build`의 `libzlink.so.0.17.0`(Release+LTO), 모든 러너에 `ZLINK_CORE_SOURCE=local`.
  release asset이 발행되면 계획서 §1대로 `--core-version 0.17.0`으로 전환하고 C 기준값을 한 번 다시 잰다.
- inventory gate(계획서 §4): 언어별 runner가 등록한 pattern/transport 목록을 뽑아 계획서 §9 상세 표와 일치시킨다. 등록되지 않은
  pattern은 표에서 제외하고 그 사실을 §11에 한 줄 남긴다. 이 gate는 codex sol medium job 하나(읽기·표 정리)로 처리한다.
- 재현 환경 manifest(계획서 §6): CPU/메모리(11 GB)/커널/컴파일러/런타임 버전을 `log/2026-09-xx-environment.md`에 기록한다.
- 측정 중 규칙: 한 번에 perf 프로세스 하나, 다른 빌드·job·사용자 perf 없음(load average를 결과와 함께 기록), `--pin-cpu` 미사용.

### 6.2 단위와 순서 (계획서 §7.0·§7.4)

- 언어 순서: C++ → .NET → Java → Node → Go → Rust → Python(계획서 §7.4-3). 한 번에 한 언어, 한 pattern, 한 transport.
- 한 (언어, pattern, transport) 단위의 절차:
  1. C 해당 pattern smoke → 같은 조건으로 binding smoke(계획서 §7.1).
  2. C 전 size 측정 직후 binding before 측정(paired, 계획서 §7.3). 결과 report 경로와 비율을 계획서 §9 표에 기록.
  3. 미달 셀이 있으면 **자체 개선 pass**: 가이드 §2 체크리스트(즉시 성공 경로 무할당·무복사, 거절 경로 O(1) 등록, drain/wake spin
     없음, 수신 direct reply)를 코드로 판정하고 수정 → after 1회 측정. 이 pass는 codex sol high job(브리프는 `bindings-review-template.prompt`
     형식에 pattern·transport·미달 셀·before 수치를 넣어 생성, 3분 생존 감시).
  4. **2차 pass**: read-only 리뷰(codex sol high 또는 Claude 서브에이전트)로 계약 보존 후보를 받아 적용 → after 1회 측정. 후보가 없으면
     no-go를 `log/`에 기록(계획서 §7.4-11).
  5. 대표 셀 회귀 gate + 표준 테스트·샘플·perf 스모크(계획서 §7.4-13, §7.6) → 채택한 변경만 커밋·push.
  6. aggregate 통과여도 POSDDD·hot-path 검토 1회(계획서 §7.4-14) → 채택 시 별도 커밋.
  7. transport 완료 기록 → 다음 transport; pattern의 모든 transport 확정 → 계획서 §9·§11 갱신 커밋·push → 다음 pattern(§7.4-15~18).
- 언어 전환 gate(계획서 §7.5·§7.4-19~20): 해당 언어의 Single·Multi 모든 pattern이 완료 또는 `보류`로 확정되고 report와 표가 대조된 뒤에만
  다음 언어로 간다.

### 6.3 판정·기록 (계획서 §2·§8·§11)

- 목표 비율과 latency 목표는 계획서 §2.1·§2.2 표를 그대로 쓴다(이 문서에 복제하지 않는다). 셀 판정은 C 대비 throughput 비율과 평균
  latency, aggregate는 transport별 size 비율의 평균.
- 측정 표에는 계획서 §8 형식으로 report 경로·조건·load average·판정만 남기고, 실행 명령·프로파일·후보 검토는 `doc/perf/perf/bindings-0.17.0/log/`에
  날짜·언어별 파일로 기록한다. 5% 이상 효과가 확인된 기법은 가이드 §3 표에도 반영한다.
- 러너 자체의 변경(scheduler·drain·fairness)은 library 최적화 효과와 합산하지 않는다(계획서 §5, 가이드 §5).

### 6.4 예상 소요와 병렬성

- 한 언어당 Multi 5 pattern × 4 transport × 5 size + Single을 paired로 재면 측정만 2~3시간, 개선 pass가 붙으면 언어당 반나절이다.
  측정은 직렬(정책)이지만 개선 pass의 코드 작업은 측정하지 않는 시간에 다른 언어의 리뷰 job과 병렬로 진행할 수 있다.
- 우선순위는 A가 쓰는 순서(C++·.NET·Java·Node)와 계획서 순서가 같으므로 그대로 따른다.

### 6.5 완료 조건 (계획서 §12)

계획서 §12의 조건(모든 언어의 Single·Multi pattern이 통과 또는 근거 있는 보류, §9·§10·§11 표가 report와 일치, 채택한 변경이 모두
push됨)을 만족하면 이 캠페인의 bindings 성능 항목을 닫고, 결과 요약을 `c016-worklog/decisions.ko.md`에 D-B 항목으로 남긴다.
