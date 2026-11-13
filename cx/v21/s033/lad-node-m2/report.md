# Node L1 permit 비용 확인 (#4351)

기준 HEAD: `c512887606aeb686eccc53162d9f02c3c4db63ed`. 기존 L1 단계 코드는 수정하지 않았다. 선행 결과와 호출 형태는 [lad-node-m1/report.md](/root/jobs/fx/cx/v21/s033/lad-node-m1/report.md), [call-shape.md](call-shape.md)에 있다. 이번 문서는 재측정 및 permit 프로파일 결과를 기록한다.


## 첫 확인 hold — 결과

2026-11-13 UTC, 같은 hold에서 각 run을 시작 시각과 함께 기록했다([progress.md](progress.md)). 모든 완료 결과는 errors=0이고 submitted=completed=received였다. G5는 3회 중앙값에서 최대 편차/중앙값이며 10% 이하는 비교 판정에 유효하다.

| 셀 | L0 중앙값 (G5) | L0R 중앙값 (G5) | L1 중앙값 (G5) | L0R/L0 손실 | L1/L0R 손실 | L1/L0 손실 | 판정 (모듈/permit/단계) |
|---|---:|---:|---:|---:|---:|---:|---|
| send-saturation-4096 | 77,428.4 (12.78%) | 85,963.6 (1.49%) | 75,917.8 (17.00%) | -11.02% | 11.69% | 1.95% | 판정 불가 / 판정 불가 / 판정 불가 |
| request-serial-1024 | 7,939.6 (6.53%) | 7,744.0 (37.97%) | 7,904.0 (3.34%) | 2.46% | -2.07% | 0.45% | 판정 불가 / 판정 불가 / 통과 |
| request-serial-4096 | 7,776.4 (16.23%) | 6,585.8 (19.88%) | 6,960.8 (6.54%) | 15.31% | -5.69% | 10.49% | 판정 불가 / 판정 불가 / 판정 불가 |

원시 결과 파일 (각 수치의 세 run):
- `send-saturation-4096`:
  - L0: [r144](progress.md#L144), [r609](progress.md#L609), [r1074](progress.md#L1074)
  - L0R: [`r1`](raw/session/confirm/send-saturation-4096/L0R/run1/run1/zlink-framework-node-send-saturation-4096/results.json), [`r2`](raw/session/confirm/send-saturation-4096/L0R/run2/run1/zlink-framework-node-send-saturation-4096/results.json), [`r3`](raw/session/confirm/send-saturation-4096/L0R/run3/run1/zlink-framework-node-send-saturation-4096/results.json)
  - L1: [`r1`](raw/session/confirm/send-saturation-4096/L1/run1/run1/zlink-framework-node-send-saturation-4096/results.json), [`r2`](raw/session/confirm/send-saturation-4096/L1/run2/run1/zlink-framework-node-send-saturation-4096/results.json), [`r3`](raw/session/confirm/send-saturation-4096/L1/run3/run1/zlink-framework-node-send-saturation-4096/results.json)
  - binding: [`r1`](raw/session/confirm/send-saturation-4096/binding/run1/run1/zlink-node-send-saturation-4096/results.json), [`r2`](raw/session/confirm/send-saturation-4096/binding/run2/run1/zlink-node-send-saturation-4096/results.json), [`r3`](raw/session/confirm/send-saturation-4096/binding/run3/run1/zlink-node-send-saturation-4096/results.json)
  - paired L0 for binding: [`r1`](raw/session/confirm/send-saturation-4096/L0/run1/run1/zlink-framework-node-send-saturation-4096/results.json), [`r2`](raw/session/confirm/send-saturation-4096/L0/run2/run1/zlink-framework-node-send-saturation-4096/results.json), [`r3`](raw/session/confirm/send-saturation-4096/L0/run3/run1/zlink-framework-node-send-saturation-4096/results.json)
- `request-serial-1024`:
  - L0: [`r1`](raw/session/confirm/request-serial-1024/L0/run1/run1/zlink-framework-node-request-serial-1024/results.json), [`r2`](raw/session/confirm/request-serial-1024/L0/run2/run1/zlink-framework-node-request-serial-1024/results.json), [`r3`](raw/session/confirm/request-serial-1024/L0/run3/run1/zlink-framework-node-request-serial-1024/results.json)
  - L0R: [`r1`](raw/session/confirm/request-serial-1024/L0R/run1/run1/zlink-framework-node-request-serial-1024/results.json), [`r2`](raw/session/confirm/request-serial-1024/L0R/run2/run1/zlink-framework-node-request-serial-1024/results.json), [`r3`](raw/session/confirm/request-serial-1024/L0R/run3/run1/zlink-framework-node-request-serial-1024/results.json)
  - L1: [`r1`](raw/session/confirm/request-serial-1024/L1/run1/run1/zlink-framework-node-request-serial-1024/results.json), [`r2`](raw/session/confirm/request-serial-1024/L1/run2/run1/zlink-framework-node-request-serial-1024/results.json), [`r3`](raw/session/confirm/request-serial-1024/L1/run3/run1/zlink-framework-node-request-serial-1024/results.json)
- `request-serial-4096`:
  - L0: [`r1`](raw/session/confirm/request-serial-4096/L0/run1/run1/zlink-framework-node-request-serial-4096/results.json), [`r2`](raw/session/confirm/request-serial-4096/L0/run2/run1/zlink-framework-node-request-serial-4096/results.json), [`r3`](raw/session/confirm/request-serial-4096/L0/run3/run1/zlink-framework-node-request-serial-4096/results.json)
  - L0R: [`r1`](raw/session/confirm/request-serial-4096/L0R/run1/run1/zlink-framework-node-request-serial-4096/results.json), [`r2`](raw/session/confirm/request-serial-4096/L0R/run2/run1/zlink-framework-node-request-serial-4096/results.json), [`r3`](raw/session/confirm/request-serial-4096/L0R/run3/run1/zlink-framework-node-request-serial-4096/results.json)
  - L1: [`r1`](raw/session/confirm/request-serial-4096/L1/run1/run1/zlink-framework-node-request-serial-4096/results.json), [`r2`](raw/session/confirm/request-serial-4096/L1/run2/run1/zlink-framework-node-request-serial-4096/results.json), [`r3`](raw/session/confirm/request-serial-4096/L1/run3/run1/zlink-framework-node-request-serial-4096/results.json)

send-saturation 4K의 paired L0/binding 중앙값 비율은 0.8994 (L0 78,145.4 / binding 86,888.8)이다. paired L0 G5 13.07%, binding G5 10.87%라 0.95 기준 판정은 불가다. 오늘 binding daily 기준 87,063.2 msg/s([binding-daily/node-2026-11-13.json](../binding-daily/node-2026-11-13.json)) 대비 L0 중앙값은 0.8976이다. paired 세 run의 L0 G5가 13.07%라 이 비율도 판정 불가다.

**원시 파일 경로 충돌:** paired L0 세 run이 ladder L0와 같은 경로를 사용해 ladder의 send-saturation 4K L0 JSON 세 개를 덮었다. ladder L0의 세 처리량/건수와 시작 시각은 링크한 progress 항목에 남아 있어 계산에는 이를 썼고, paired L0 및 다른 모든 JSON은 보존돼 있다. 재발 방지를 위해 프로파일 출력은 별도 디렉터리를 사용한다.

**해석:** 기존의 request-serial 1K 단계 손실 43.8%는 이번 중앙값에서 0.45%로 재현되지 않았다(단계 비교 G5 유효). request-serial 4K는 단계 손실 10.49%로 7%를 넘지만 L0 G5 16.23%로 판정 불가다. 다만 요청대로 L0/L1 paired profile을 수집한다. request-serial 1K와 4K의 L0R/L0 중앙값 손실은 각각 2.46%, 15.31%이며 G5가 유효하지 않아 두 쌍의 module profile도 함께 수집한다.
