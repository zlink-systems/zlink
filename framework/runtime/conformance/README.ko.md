# Framework runtime conformance fixture

이 디렉터리는 service wire와 무관한 Framework 내부 불변식을 언어 중립 JSON으로 고정한다.
각 언어 runtime은 같은 fixture를 직접 읽는 white-box test를 두며, 구현 언어의 thread,
executor, lock과 event-loop 선택은 fixture에 넣지 않는다.

`serial-execution-v1.json`은 owner별 serial execution의 admission, lane arbitration과
same-owner 호출 규칙을 정의한다. 수치나 trace를 바꾸려면 C++/.NET/JVM/Node 소비 test와
common internals를 같은 변경 단위에서 갱신해야 한다.

`runtime-observation-v1.json`은 subscriber별 source-latest intermediate, bounded terminal
FIFO, source key 수명과 서로 분리된 loss counter를 정의한다. Source 사이의 공개 sequence
비교를 만들지 않으며, fixture의 terminal 배열은 terminal FIFO 안의 순서만 고정한다.

검증 명령은 다음과 같다.

```bash
node framework/runtime/conformance/validate-runtime-conformance-fixtures.mjs
```
