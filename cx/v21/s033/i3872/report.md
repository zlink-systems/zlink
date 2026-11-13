# Node 셀 재측정 (#3872, main)

2026-11-11, `HEAD=76a4d7850664c2cb32e8d35837dac45d9d2a4057`에서 Node v22.23.3으로 측정했다. `scripts/local-package/build-wsl.sh node`, `scripts/local-package/http-client/build-wsl.sh node`, `framework/bench/grpc/node/build.sh`가 모두 완료됐다. Node binding package에는 `prebuilds/linux-x64/zlink.node`가 포함됐고 C++ 소스 빌드는 없었다 (`node-binding-build.log`, `http-client-build.log`, `bench-build.log`).

각 셀은 warmup 30초, active 5초, 3회였다. 매 회차에서 Framework와 binding 순서를 바꿔 실행했다. 조건 B는 source CPU 0–7, target CPU 8–15였다. 이 조건을 위한 `framework/bench/grpc/node/run_local.sh` 변경은 측정 후 원상 복구했다. 모든 측정은 `flock -o /root/jobs/fx/cx/measure.lock` 안에서 실행했다 (`measure-sequence.log`).

| 셀 | Framework 회차 msg/s (중앙값; 범위) | Binding 회차 msg/s (중앙값; 범위) | 중앙값 F/B | 회차별 F/B 범위 |
|---|---:|---:|---:|---:|
| send-saturation 1024 | 74,081.2, 75,469.2, 76,071.4 (75,469.2; 74,081.2–76,071.4) | 115,898.0, 114,800.8, 111,209.4 (114,800.8; 111,209.4–115,898.0) | 0.6574 | 0.6392–0.6840 |
| send-saturation 4096 | 57,095.2, 57,097.0, 54,407.6 (57,095.2; 54,407.6–57,097.0) | 90,043.8, 89,770.0, 86,777.4 (89,770.0; 86,777.4–90,043.8) | 0.6360 | 0.6270–0.6360 |
| request-serial 1024 | 5,526.2, 5,183.8, 5,067.4 (5,183.8; 5,067.4–5,526.2) | 8,441.0, 8,470.4, 9,033.0 (8,470.4; 8,441.0–9,033.0) | 0.6120 | 0.5610–0.6547 |
| request-serial 4096 | 4,963.6, 5,011.6, 5,002.8 (5,002.8; 4,963.6–5,011.6) | 8,098.8, 8,015.2, 7,494.4 (8,015.2; 7,494.4–8,098.8) | 0.6242 | 0.6129–0.6675 |
| request-backpressure 1024 | 16,563.2, 16,972.8, 16,588.8 (16,588.8; 16,563.2–16,972.8) | 27,133.2, 26,809.6, 27,014.8 (27,014.8; 26,809.6–27,133.2) | 0.6141 | 0.6104–0.6331 |

중앙값 F/B는 Framework 처리량 중앙값을 binding 처리량 중앙값으로 나눴다. 회차별 범위는 같은 회차 두 처리량의 비율이다. send-saturation 4096은 중앙값과 전체 회차 범위 모두 0.6보다 높고, 범위가 0.6을 포함하지 않는다. 따라서 요청 조건에 따른 추가 TID-union CPU 측정 및 Framework 4K CPU profile은 실행하지 않았다.

원본 30개 `results.json`과 셀별 stdout/stderr는 `/root/jobs/fx/cx/v21/s033/i3872/raw/i3872-r*-*/run1/*/`에 보존했다. 회차 실행 기록은 `/root/jobs/fx/cx/v21/s033/i3872/measure-sequence.log`다. 빌드 원본 로그는 같은 job 디렉터리의 `node-binding-build.log`, `http-client-build.log`, `bench-build.log`다. Framework 빌드 산출물과 설치 의존성은 제거했고, worktree의 `git status --short`는 깨끗하다.
