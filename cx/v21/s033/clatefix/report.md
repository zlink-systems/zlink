테스트 파일만 수정했다. `test_cpp_framework_actor_late_result.cpp:85-90`에서 timed_out terminal 전에 owner lane turn을 최대 2초 기다려 비동기 deadline index 게시를 읽은 뒤 확인한다. 이전의 무제한 `get()`은 bounded `wait_for()`로 바꿨다. `:913-915`와 `:925-927`에서는 제출 및 completion을 각각 최대 2초 기다린다. completion은 실패 결과도 유효한 terminal이므로 성공 여부가 아니라 준비 여부만 확인한다. 제품 코드는 바꾸지 않았다.

수정 전 부하 재현은 focused 테스트 44회 중 hang 3회였고 실패 결과는 0회였다. 기록은 `/root/jobs/fx/cx/v21/s033/clatefix/before-summary.txt`에 있다. 수정 후 최종 실행 파일은 focused 테스트 2,000회 중 hang 0회, 실패 0회였고 whole binary 300회 중 hang 0회, 실패 0회였다. 기록은 `/root/jobs/fx/cx/v21/s033/clatefix/after-final-summary.txt`에 있다.

framework unit 및 contract CTest는 291개 모두 통과했고 실패와 미실행은 0개였다. 최종 실행 기록은 `/root/jobs/fx/cx/v21/s033/clatefix/ctest-framework-unit-contract.log`에 있다. 누락된 세 connector executable을 찾기 전의 불완전한 실행은 중단했으며 기록은 `/root/jobs/fx/cx/v21/s033/clatefix/ctest-framework-unit-contract-incomplete.log`에 보존했다. C++ format check와 `git diff --check`는 통과했다.

중복 대기와 handoff를 점검했다. `:85-90`의 owner lane barrier와 deadline 확인 및 expire 결과 대기는 각각 index 게시 순서와 timeout 적용을 확인하므로 모두 유지했다. `:622-633`의 dispatch poll은 요청을 application record로 전달받는 대기라 제출 결과 대기와 다르므로 유지했다. `:913-915`의 제출 대기와 `:925-927`의 completion 대기는 서로 다른 terminal 지점이므로 유지하되 두 무제한 결과 대기는 각각 2초 제한으로 바꿨다. `:934-935`의 `native_reply(nullopt)`는 held handler turn 정리라 유지했다. 중복 대기나 handoff를 삭제한 항목은 없다.

변경 파일은 `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_actor_late_result.cpp` 하나다. 최종 diff는 `/root/jobs/fx/cx/v21/s033/clatefix/change.diff`에 저장했다.
