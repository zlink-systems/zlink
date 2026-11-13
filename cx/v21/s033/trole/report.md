# Target receive-role 인계 진단·설계 — #1639

회전 종료 뒤 기존 executor가 이미 생성된 idle worker에 수신 역할을 독점 배정하고, 이전 수신자가 정상 owner drain에 진입하는 제한 실험을 제안했다. 실패 시 기존 경로를 유지한다. [설계 §6](design.md:121)

- 소유 계층: C++ Framework offload executor. 배치 모듈 소유자 **1→1**, 결정 판정 위치 **12→11 제안**이다. 실제 제품 변경은 없어 현재 수는 **12→12**다. [결정 소유 표](design.md:94)
- Spec: S08 §4.4 전체 규칙·표 5행과 chand §4 수락 조건을 검증 계획에 연결했다. [원문 인용](design.md:23), [검증 매핑](design.md:149)
- 교차언어: .NET **6821c3b8af**의 미시작 application 예약 교환과 Java core·Node 경로를 파일:줄로 대조했다. .NET의 worker 검색·blocking lock은 복제하지 않는다. [대조 표](design.md:77)
- 분류: 제안은 **A(기존 계약 적응)**다. 현재 prefix가 같은 Node/Channel의 독립 실행을 막는 **B 후보**는 PM에게 함께 제출할 차단 사항이다. [선행 조건](design.md:190)

변경은 job 산출물 `design.md`, `report.md`와 `logs/`뿐이다. 제품·spec diff **0**, 빌드 **0회**, 기능 시험·성능 측정 **0회**, sub-agent·다른 AI 호출 **0회**다. Commit·push·PR은 수행하지 않았다. [최종 상태](logs/final-repository-state.txt)

문서 검사: 직접 파일·줄 참조 **154/154**, S08 인용 **21/21** 확인. 잘못된 경로와 변조 인용을 넣은 음성 대조 **2/2 검출**이다. 기능 시험 통과를 뜻하지 않는다. [정상 검사](logs/design-validation.json), [음성 대조](logs/design-validation-negative.json)

남은 위험은 같은 owner의 기존 prefix 차단, 인계 실패·종료 경합의 실제 검증, 성공률과 통지 포함 CPU·sendto 개선의 미확정이다. 12개 검증 항목과 같은 세션 base/candidate 3쌍의 측정·중단 기준을 제안했다. 감독자가 소유 표와 차단 조건을 PM에게 전달한 뒤 구현 승인을 판단한다. [시험](design.md:172), [측정](design.md:192)

리팩토링 점검: 성능·소유 후보 **3건**, 실제 수정 **0건**, 제안 포함 **2건**, 같은 작업의 PM 차단 사항 **1건**이다. [점검 근거](design.md:225)

시작 점검의 `internal-docs.sh`가 문서 저장소 `git pull`을 1회 수행한 것은 이번 GitHub 금지 지시 위반이었다. 이후 원격 작업은 하지 않았다. [실행 기록](logs/startup-exception.txt:1)
