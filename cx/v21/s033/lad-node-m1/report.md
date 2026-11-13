# Node L0R + L1 빠른 측정

기준 HEAD: `c512887606aeb686eccc53162d9f02c3c4db63ed` (L0 경로 정합성, L1 permit, L0R module-load switch 포함). 코드 변경 없음: 실험 코드는 이미 로컬 커밋 상태다.

호출 형태 대조는 [call-shape.md](call-shape.md)에 기록했다. 측정 전에 커밋과 산출물 해시, binding/L0/L0R/L1 receive·reply 코드 위치를 `progress.md`에 기록한다.

측정 결과는 lock hold 종료 직후 여기에 추가한다.

## 진행 중 첫 수치

| 시나리오 | 크기 | binding msg/s | 오류·완료 일치 | 원시 파일 |
|---|---:|---:|---|---|
| send-saturation | 1024 | 110892.6 | errors=0, submitted=completed=received=561108 | `raw/session/binding/run1/zlink-node-send-saturation-1024/results.json` |
