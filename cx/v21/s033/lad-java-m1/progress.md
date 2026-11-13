# 측정 진행

- Branch `exp/4350-java-ladder`, HEAD `23be466e71c126000c874c73dda15d1495435aed`, 시작 시 clean.
- 2026-11-13 UTC binding 참조 생성: lad-java-par fresh L0의 `zlink-java` 6셀, 3회 중앙값. 셀별 값과 세 raw 경로는 `/root/jobs/fx/cx/v21/s033/binding-daily/java-2026-11-13.json`에 저장.
- Call-shape 표: `call-shape.md`. 측정 전 네 호출 구간 모두 일치; 단계별 허용 차이만 확인.
- 빌드 첫 시도는 load average 15.84에서 build guard에 의해 중단됨. 다른 작업의 `measure.lock` 보유 및 load average <10 조건 해제를 기다리는 중.
