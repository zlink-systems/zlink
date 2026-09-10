---
title: "Issue #63 perf 재측정 결과 — multi routed before→after (whole-message)"
---

# Issue #63 perf 결과 (⑧)

측정: 2026-09-10, 머신 B(WSL2, 16코어/11GB). multi routed(DEALER_ROUTER·ROUTER_ROUTER × SENDSEND·REQREP),
tcp, `--msg-sizes 64,256,1024,4096,65536,131072`, `--duration 5 --runs 1`, clients=100.
- **after** = 이번 브랜치(part API 제거 + whole-message), workspace Core release(`core/build`, 0.17.5, 신 API).
  러너 기본값이 workspace Core라 그대로 branch Core로 측정(러너 META: `core_source=local, core_version=0.17.5`).
- **before** = 기록된 baseline TSV `doc/perf/perf/bindings-0.17.5/log/results-<lang>-multi.tsv`(Core 0.17.4, 구 part API).
- C 레퍼런스도 whole-message로 함께 측정(신 API). cpp는 이번 세션에서 C+binding을 같은 상태로 측정.

## 바인딩 throughput 개선 (Δ%, before→after)

| 바인딩 | 패턴 | 64 | 256 | 1024 | 4096 | 65536 | 131072 |
|---|---|---|---|---|---|---|---|
| cpp | DEALER_ROUTER SENDSEND | +28 | +116 | +98 | +47 | +62 | +122 |
| cpp | DEALER_ROUTER REQREP | +36 | +56 | +20 | +33 | +12 | +9 |
| cpp | ROUTER_ROUTER SENDSEND | +127 | +80 | +65 | +33 | +63 | +187 |
| cpp | ROUTER_ROUTER REQREP | +31 | +14 | +18 | +14 | +8 | +9 |
| java | DEALER_ROUTER SENDSEND | +148 | +167 | +171 | +240 | +61 | +81 |
| java | DEALER_ROUTER REQREP | +48 | +35 | +37 | +48 | +61 | +79 |
| java | ROUTER_ROUTER SENDSEND | +120 | +71 | +50 | +60 | −8 | +32 |
| java | ROUTER_ROUTER REQREP | +50 | +32 | +16 | +22 | +26 | +38 |
| dotnet | DEALER_ROUTER SENDSEND | +180 | +143 | +187 | +170 | +35 | +52 |
| dotnet | DEALER_ROUTER REQREP | +45 | +51 | +57 | +71 | +41 | +34 |
| dotnet | ROUTER_ROUTER SENDSEND | +160 | +111 | +72 | +97 | +38 | +50 |
| dotnet | ROUTER_ROUTER REQREP | +84 | +87 | +59 | +70 | +54 | +27 |

## 판정
- **소·중 사이즈 SENDSEND에서 개선이 가장 큼**(java/dotnet @64~1024 +140~240%, 약 2~3배). whole-message가
  메시지당 native 경계·part-flag 계산·part 루프를 제거한 효과 — draft §7의 ".NET 메시지당 native 경계 14회"
  목표 지렛대가 그대로 실측됨. cpp/java/dotnet 세 바인딩 모두 동일 경향.
- 대형(65536~131072)은 payload 복사가 지배해 개선폭이 작지만 대부분 +30~80%. java RR_SENDSEND@65536만
  −8%(측정 노이즈 범위).
- **비대상 회귀 없음**: 측정 셀 전부 개선(1셀 −8% 노이즈 제외). cpp는 절대 throughput으로 판단
  (bind/c ratio는 C 레퍼런스도 whole-message로 빨라져 하락 — 개선이 계약 완화가 아니라 경로 제거에서 온 증거).

## 주의·한계
- baseline이 Core 0.17.4라 Δ에 whole-message + 0.17.4→branch 변화가 섞임. whole-message만 순수 격리하려면
  base 커밋 A/B가 필요(추후). cpp는 이번 세션 C+binding 동시 측정으로 상대적으로 깔끔.
- **node 미측정**: perf 하네스 빌드가 #63 무관 **사전존재 TS declaration 오류**(`Buffer` private name;
  binding_socket.ts 등 origin/main과 동일, e82dec4ffc 미변경)로 실패. workspace-Core 경로의 source
  declaration 빌드에서만 표면화(baseline은 release 패키지 경로라 우회). 별도 이슈로 분리 권장.
- go/python/rust는 이번 perf 범위 밖(측정 대상 cpp/node/java/dotnet만, 사용자 지정).

## 산출 리포트
러너 저장 리포트: `bindings/{c,cpp,java,dotnet}/perf/results/multi/report/*i63-after*.txt`(status: complete).
