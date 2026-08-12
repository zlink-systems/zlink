  > 현재 main을 기준으로 C++, .NET, JVM(Java/Kotlin), Node Framework runtime과 unit test를 끝까지 리팩터링한다.
  >
  > - common spec과 언어별 exact interface를 read-only 계약 기준으로 사용한다.
  > - spec, common internals 등 모든 보호 문서는 수정하지 않는다.
  > - public API, wire schema, lifecycle·ownership 의미를 변경하지 않는다.
  > - POSDDD 원칙으로 production과 unit test를 함께 검토하고, 책임 이동·중복 정책·pass-through wrapper·dead code·test-only 우회를 정리한다.
  > - 성능 finding은 기존 benchmark로 전후를 비교할 수 있을 때만 수정한다. 측정 근거가 없으면 추정으로 최적화하지 말고 보고한다.
  > - 중복 test를 정리하더라도 scenario와 assertion coverage는 줄이지 않는다.
  > - 계약을 유지하는 것으로 검증된 finding은 실제 코드와 test에 반영한다. 단순 검토로 끝내지 않는다.
   > - 각 언어에서 focused test와 전체 unit regression을 filter·skip 없이 실행한다.
  > - 한 언어에서 실패하면 원인을 해당 언어 범위에서 해결하고 그 언어의 전체 gate를 다시 실행한 뒤 다음 언어로 넘어간다.
  > - 공통 계약 불일치, spec 해석 문제, 새 public contract 필요, 언어 제약으로 인한 의미 차이가 발견되면 해당 내용을 임의로 수정하지 않는다. 추가 수정과 commit·push를 중단하고 file:line, 관찰된 동작, 계약 근거, 선택지를 보고한다.
  > - 마지막에 다섯 언어의 공개 동작을 state transition, ordering, concurrency, ownership, error, bound 기준으로 다시 비교한다.
  > - 모든 언어의 직렬 test와 최종 cross-language 리뷰가 통과하고 미해결 finding이 없을 때만 RUNTIME-UNIT-CLEAN으로 판정한다. 그렇지 않으면 NOT CLEAN 또는 BLOCKED로 판정한다.
  > - 각 언어 checkpoint를 검증 후 별도 commit으로 만들고 main에 순차 push한다. 마지막에는 worktree clean, upstream divergence 0 0, local/remote SHA 일치를 확인한다.

