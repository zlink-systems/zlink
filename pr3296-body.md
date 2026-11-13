Closes #3296

## 원인
`MeshLivenessDisconnectContractTests`의 ControlSend fixture에서 raw DEALER가 연결 준비 전에 첫 HELLO를 제출할 수 있습니다. 이 소켓에 공개 completion owner가 없어, 비동기 backpressure 경로가 시험하려는 control-send 실패를 주입하기 전에 `InvalidState`(zlink error code 8)로 끝났습니다. 분류: 시험 fixture 결함, 제품 코드 변경 없음.

## 수정
기존 `TestCompletionPollerDriver`를 peer 소켓에 붙여(1줄), 기존 admission await가 공개 poller로 완료를 관찰합니다. deadline·단언은 그대로입니다.

## 검증
- 부하(`taskset -c 8-15`, `stress-ng --cpu 8`)에서 ControlSend 시험 3건 150회: 수정 전 4/150 실패 → 수정 후 0/150

### 로컬 .NET 시험 (pre-pr.sh, HEAD a844abc130, main e40bff6c43 병합)
| 구성 | Unit | Contract | Stream | 결과 |
|---|---|---|---|---|
| Debug net8.0 | 4129 | 98 | 269 | 통과 |
| Release net10.0 | 4127 | 98 | 269 | 통과 |
- Debug·Release 빌드, format.sh --check dotnet 통과

🤖 Generated with [Claude Code](https://claude.com/claude-code)
