# Core 0.17.1 bindings 성능 측정 환경 (공식 기준)

기록일 2026-09-07. 이 manifest가 이 캠페인의 **공식 측정 환경**이다(D-BP5·D-BP7).
같은 폴더의 `2026-09-07-environment.ko.md`는 Core 0.17.0 local artifact 기준이며 러너
정합 작업의 smoke 전용이었다. 그 artifact로 얻은 수치는 기준값이나 판정 근거로 쓰지 않는다.

## Source와 Core runtime

| 항목 | 값 |
|------|----|
| source | `main` / `074d2a596470dcf2899cd3060858cdd8056ac12e` |
| 작업 트리 상태 | `framework/**`에 이 캠페인과 무관한 잔여 변경이 있다. `core/`·`bindings/`·`doc/perf/`는 clean |
| Core 버전 | 0.17.1 (`VERSION`, `core/CMakeLists.txt:11`, `core/include/zlink.h:8-10` 3곳 일치 확인) |
| 원격 릴리스 태그 | `core/v0.17.1` (범프 커밋 `4cd03b9173`) |
| Core runtime | `/home/hep7/project/zlink/core/build/lib/libzlink.so.0.17.1` |
| Core build | `scripts/build-core.sh release` — `CMAKE_BUILD_TYPE=Release`, `ENABLE_LTO=ON`, `ZLINK_BUILD_TESTS=OFF` |
| ELF Build ID | `f7e2a5397eb55df4001f5a447f4ca0522be09e7c` |
| SHA-256 | `79cc435817797cbfbe5ff2ad105915233bdb18c37336044e3b0aaad2bde3850c` |
| 크기 | 6,507,544 bytes |
| provenance | 이 작업영역에서 빌드한 local artifact. GitHub release asset이 아니다 |

**`--core-version 0.17.1` 사용 금지.** 모든 러너는 `ZLINK_CORE_SOURCE=local`로 위
`core/build` 트리를 직접 쓴다. `~/.cache/zlink/core/` 아래에는 `core/build-dev` 유래의
RelWithDebInfo/LTO OFF artifact가 있어 `--core-version`을 쓰면 그쪽이 선택된다.
직전 0.17.0 artifact는 혼동을 막기 위해 이 트리에서 제거했다.

## Host

| 항목 | 값 |
|------|----|
| 환경 | Linux x86_64, WSL2 |
| kernel | `6.6.87.2-microsoft-standard-WSL2` |
| CPU | Intel(R) Core(TM) Ultra 7 265K |
| 논리 CPU | 20 |
| memory | 94 GiB |
| CPU governor | WSL2에서 미노출 |
| 홈 | `/home/hep7` |

> 2026-09-05/06 측정은 다른 호스트(16 논리 CPU, 11.7 GiB, 홈 `/home/hep7hep7`)에서
> 수행됐다. 그 값들은 참고값이며 이 환경의 판정 근거로 쓰지 않는다.

## 상류 변경 (이 기준값에 영향)

- `7549a128b1` — `bindings/c/perf/common/perf_zlink_part_helpers.hpp`의
  `perf_measurement_part_count()`가 send/recv마다 부르던 `getenv`를 함수 스코프
  `static const`로 캐시. RR single 축소셀 Ir/msg 15,806 → 14,324(−9.4%). C 기준값은
  이 커밋 이후로만 유효하다.
- `2753a2d799` — Core Phase 3 R5+R6R8+R9+R7R11 정리(−1833/+561).
- `074d2a5964` — C single REQREP 러너 복원 + 단일 active phase 통합.
- `d634417a37` — 7개 binding multi 종료 protocol·부하 수준 정합.

## 명령

```bash
scripts/build-core.sh release
ZLINK_CORE_SOURCE=local bindings/c/perf/run_benchmarks_multi.sh --pattern <P> --transports <T> ...
ZLINK_CORE_SOURCE=local bindings/<lang>/perf/run_benchmarks_multi.sh --pattern <P> --transports <T> ...
```

## 알려진 환경 제약

- `python3`에 `pytest`가 없다. 러너 테스트는 `python3 -m unittest`로 실행한다.
- `~/.cache/zlink/python-test-venv`가 없다. Python binding 측정 전에 준비한다.
- Java는 perf 전용 JDK 없이 시스템 Temurin 하나뿐이다. Java 측정 전에 확인한다.
- WSL2 wall clock이 ±5초 점프한다(D-095). 측정은 monotonic clock만 쓴다
  (`PERF_POLICY.md` §1.1). **미해결**: multi metric header 시간원이 C만 monotonic이고
  C++·.NET·Python은 wall clock 기반이다 — 러너 정합 6b 항목.
