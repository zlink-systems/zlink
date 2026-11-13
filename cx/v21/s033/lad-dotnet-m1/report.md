# .NET L0/L1/L2 측정

- 실험 브랜치 `exp/4350-dotnet-ladder`; L0 header parity fix `ab21f1eb31`, L2 envelope header `2590b2a030`, scheduler capture `bfc15a8dd6` 로컬 커밋.
- [call-shape.md](call-shape.md)에 L0/L1/L2와 binding의 네 경로 및 메시지당 비용을 기록했다.
- Release solution build 통과. Client, server, raw server, shared, Framework DLL은 관련 소스보다 최신이다.
- 새 `common-rules.md`의 스케줄 정책 기록을 적용했다. ladder hold는 A/A(`aa-noise3`) G5 확인 전에는 시작하지 않는다.

| 셀 | binding msg/s | L0/binding | L1/L0 손실 | L2/L1 손실 | 판정 | raw |
|---|---:|---:|---:|---:|---|---|
| 1K/4K 6셀 | 미측정 | — | — | — | A/A gate로 중단 | — |

## A/A 측정 gate 상태 (2026-11-13 17:04 UTC)

`aa-noise3` request-serial 4K에서 A G5 32.4%, B G5 14.1%, A/B 중앙값 비율 0.946으로 기준 5%를 넘겼다. send-saturation 4K는 유효 run이 A/B 각 1회라 G5 판정 불가다. 따라서 측정 소음 원인이 확인될 때까지 ladder 측정을 중단한다. 관측 환경에는 `IDL` 클래스 `cc1plus`가 있었지만 정상 run에도 나타나 단일 원인으로 특정되지 않았다. 수치·raw 근거는 [A/A report](../aa-noise3/report.md)와 그 연결 파일을 참조한다.

A/A raw는 request-serial A/B 각 5회 시도 중 SCHED_OTHER 위반 1회씩을 제외한 4회, send-saturation A 2회 시도(유효 1회), B 1회로 끝났다. 이번 작업은 ladder hold를 시작하지 않아 binding daily 파일과 ladder raw가 없다.
