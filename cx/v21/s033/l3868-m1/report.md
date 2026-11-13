# #4350 C++ L0 · #4351 L1 permit 빠른 측정

소스 `608513f260`, branch `exp/3868-cpp-ladder`. [호출 대조](call-shape.md)는 HEAD와 일치한다. L0·L1은 같은 바이너리의 runtime switch로 선택하며 L1은 실제 application job queue permit만 추가한다.

조건: warmup 3초, active 5초, CPU source 0–7 / target 8–15. 기본 1회, loss 5–9% 셀은 L0/L1 3회, L0÷daily binding<0.95 셀은 binding/L0 3회로 확인한다. G5는 3회 셀에서만 spread≤10%를 적용한다.

현재 결과: build-l3868 미생성. 기존 build 요청이 shared measure gate에서 대기 중이다. [진행 기록](progress.md). 측정값은 각 hold 종료 직후 이 파일에 추가한다.
