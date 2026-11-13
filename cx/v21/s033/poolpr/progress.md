# 진행 기록

- 브랜치 `perf/1639-cpp-completion-pool`, 기준 `origin/main` `56f8984468`; 초기 작업 트리 clean.
- 진단: worker 수 판정은 `framework/languages/cpp/framework/src/runtime/dispatch/provider_task_context.cpp:56-63` 한 곳이다. 소유 규칙은 S01 §11, 독립성/자원 경계는 S08 §4.4다. 분류 B(기존 실행 자원 배치 계약에 대한 구현 조정); 결정 소유자 1 → 1, 규칙 1 → 1.
- 교차언어 대조: C++ runtime만 고정 크기 변경. 다른 언어의 runtime pool은 각 언어 소유 worker 정책이고 변경하지 않는다. 공유 spec은 callback의 독립성 및 전체 자원 점유 시 보장하지 않는 경계를 규정한다.
- 구현 시작: C++ dispatcher worker 수를 2로 고정, affinity test의 기대값 수정, operation registry에 독립 실행과 2-worker 포화 경계 테스트를 추가 중.
- negative control: production option 없이 1-worker dispatcher mutant를 만들 수 있는 기존 test/CMake 패턴이 확인되지 않아 생략 예정.
- 문서 확인: `framework/doc` 및 C++ 문서에서 completion dispatcher worker 수를 CPU 수 기반으로 명시한 문장 없음. S08 §4.4 표의 기존 경계 문장을 유지하며 테스트 주석으로 해당 제한을 명시.
- 테스트 소비자 조사: 실제 worker count 기대값이 있던 곳은 `test_cpp_framework_available_processor_count.cpp:33` 한 곳. 기존 `std::max (2u, selected)`에서 `2`로 변경. 나머지는 `0`/`!= 0` 수명 검사와 worker 수만큼의 resource occupancy로 고정 크기 가정 없음.
- 새 경계 테스트 위치 `test_cpp_framework_operation_registry.cpp:131-193`, main 등록 `:995`. 첫 callback block 중 independent callback 완료, 두 worker 각각 block 중 third terminal removal 확인, 첫 worker만 release 후 third callback 실행을 검사.
- 첫 format check가 새 lambda formatting 위반을 발견해 clang-format으로 수정; 후속 `bash scripts/format/format.sh --check cpp` 통과.
- Release configure는 초기에 `zlink_cpp` 패키지 경로를 찾지 못해 멈춤. local package README 절차에 따라 검증된 cache prefix(`zlink-cpp/1.23.0`, Core `1.23.0/linux-x64`)를 지정해 완료.
- 영향 target 빌드 92/92, 이후 framework-unit suite 전용 실행 타깃 398/398, alias 14/14 성공; build 25 GB cap 아래.
- 50회 테스트 초회는 formatter 직전 버전으로 완료. 최종 개별 50회 반복과 framework-unit full CTest 결과 대기 중.
- 최종 테스트에서 존재 확인 대기는 모두 future `wait_for(5s)`로 제한하고, latch 해제 후 worker callback 반환도 확인하도록 보강. 최종 operation registry 반복 50/50 통과.
- 최종 framework-unit 전체 CTest: 242개 중 241개 통과, `test_cpp_framework_submit_result_contract` 1개 실패. 실패는 닫힌 DEALER socket의 User Spot 오류 매핑에서 `kind=9` 관찰(`test_cpp_framework_submit_result_contract.cpp:102-105`); 변경 파일 밖.
- 최종 formatter 검사 통과. 로컬 commit `b74c1408d1` 생성.
- 정리 예정: `framework/languages/cpp/build/pool1639` 및 임시 logs 제거; 사용자 요청 산출물 `progress.md`, `report.md`는 유지.
- 임시 build 디렉터리와 로그 디렉터리 제거 완료.
