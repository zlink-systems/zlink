# Core 핫패스 규칙 (포인터)

Core 핫패스의 범위·금지 동작·상태 캐시·성능 gate는 정식 스펙
[Core hot path](../../../core/doc/spec/core/systems/10-hot-path.ko.md)가 규범으로 고정한다. 이 파일은
개발 원칙 색인에서 그 스펙으로 가는 포인터일 뿐이며, 규칙 본문을 여기에 복제하지 않는다.

요약(정본은 스펙):

- message마다 실행되는 경로(§2 표)에서는 heap 할당, 문자열로 identity 해석, socket 단위 table 조회와 그
  mutex, 조건 없는 부가 작업, reader를 재우는 미리보기, 고정 시간 sleep, 임시 owner의 신호 누락을 하지
  않는다(§3).
- message 경로가 묻는 상태는 상태가 바뀌는 지점에서 pipe에 atomic으로 게시하고, 일반 경로는 후퇴용으로만
  남긴다(§4).
- 핫패스 변경은 callgrind 명령어/message gate(`core/tests/perf/hotpath_gate`)와 release 비교 gate(cell 5% +
  size 기하평균 ≥1.0)를 모두 통과해야 한다(§5). gate 완화·이월은 하지 않는다.
