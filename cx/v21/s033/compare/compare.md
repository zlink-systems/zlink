# C++ 구조 변경 설계안 4종 비교 (#3867·#3868, 0.33.0 #1639)

입력: sol ultra(cadvice/report.md), zlink(zdesign/answer.md), kairos(kdesign/design.md), Fable(fdesign/design.md)
기준 수치: cprof, main, request-backpressure 1K, 0.6 예산 = 7.56 µs/msg/thread

## 공통 결론 (4안 모두 일치)
- 병목은 스레드 전달이 아니라 직렬 스레드 5개의 CPU 작업이다
  - source 제출 23.4 / source Core IO 19.5 / source 수신 18.4 / target Core IO 18.9 / target 수신 17.0 µs/msg
  - 제출 스레드 문맥 전환은 0.036/msg 뿐, 전달·wake 합계 1.47 µs
- 호출자 실행 제출은 이미 inline이다 — 줄일 전달이 없다
- 현재 스펙(01 §10.1·§11, 08 §4.3·§4.4, 04 §3) 안에서 C++ 1K 상한: rb 약 0.40~0.45, ss 약 0.48. 0.6은 도달 불가
- 전용 reactor 스레드는 기각(sol·kairos·Fable)

## 차이
| 항목 | sol ultra | zlink | kairos | Fable |
|---|---|---|---|---|
| 1순위 | 호출자 제출 유지, 수신 역할 인계 제한 실험 | 중복 notify 제거 | 회전당 완료 일괄 처리 | target owner drain 직렬화 해소 |
| 수신 역할 인계 | target 먼저 제한 실험 | target→source 순서 | 포함(K2) | 효과 작음 |
| 새로 찾은 사실 | 상한 rb 0.174 / ss 0.322 (호출자 기준) | 할당 7.56·bookkeeping 5.06·호출 5.00 분해 | Core IO syscall 7배 | target drain 22.8 µs/record, 0.94 core |
| 0.6 경로 | 근거 없음 | 제출 CPU 함수별 감축 필요 | lane 단위 batching(08 §3 해석 필요) | 스펙 변경(08 §4.3 W1/W5, 04 §3: worker N개가 gate 없는 owner record pop) |

## 판단
1. 다음 구현: target 전용 수신 역할 인계 실험 1건 + coreio 조사 결과(왜 Core IO가 binding의 3~8배인가)로 Core 배치 여지 확인
2. 할당·bookkeeping 감축(#3888 후속)은 상한 안에서 계속 진행
3. 0.6은 스펙 변경 없이는 불가 — 대표님 결정 필요
