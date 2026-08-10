# Framework public contract 변경 ledger

이 문서는 Framework internals 통합과 POSDDD 리팩터링을 수행하면서 public contract 또는
언어별 exact interface를 변경해야 하는 경우, 유지보수자가 변경 이유와 호환성 영향을 한
곳에서 판단할 수 있도록 실행 근거를 기록한다. 임시 작업 기록이며 정식 spec·internals·guide는
이 문서를 참조하지 않는다.

## 변경 권한과 적용 기준

사용자는 2026-08-10에 구현을 끝까지 진행하는 데 필요한 public contract와 exact interface
변경을 승인했다. 구현 차이를 감추기 위한 API는 추가하지 않는다. 현재 공개 표면으로 계약을
지킬 수 없다는 production call path와 cross-language 증거가 있을 때만 변경하며, 같은
checkpoint에서 다음 항목을 함께 처리한다.

- 공통 contract와 언어별 exact interface의 현재 목표 상태
- C++/.NET/JVM/Node public surface parity
- 기존 호출자의 source·binary 호환성 및 필요한 migration
- contract test, package gate와 sample 검증
- 변경을 포함하는 commit과 pushed SHA

## 변경 기록

현재까지 public contract 또는 exact interface를 변경한 항목은 없다. 내부 queue, relocation
protocol 구현과 white-box test 변경은 기존 공개 계약을 구현하는 작업이므로 이 표에 넣지
않는다.

| ID | 상태 | 변경 전·후 | 변경 이유와 근거 | 호환성·migration | spec·interface·test | pushed SHA |
|---|---|---|---|---|---|---|
| - | 없음 | - | - | - | - | - |

