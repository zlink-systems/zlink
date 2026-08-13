# core 0.11.0 성능 측정 기록

이 폴더에는 0.11.0 release runtime으로 실행한 C 기준과 binding의 측정 기록, 계산 결과,
개선 전후 비교만 저장한다. 후보 탐색 과정이나 장시간 작업 이력은 기록하지 않는다.

파일 이름은 `YYYY-MM-DD-언어-suite-pattern-transport.ko.md` 형식을 사용한다.

각 기록에는 다음 항목을 포함한다.

- Core와 binding package 버전, runtime provenance
- C와 binding의 실행 조건 및 원본 throughput
- message size별 C 대비 비율과 transport·pattern 산술평균
- 개선 전후 수치와 최종 판정
