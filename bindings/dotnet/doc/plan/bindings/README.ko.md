# .NET Bindings Core Alignment 문서

이 디렉토리는 `bindings/dotnet`을 최신 `core` public surface에 맞춰 정렬하는
실행 문서를 담는다.

문서:

- [`dotnet-bindings-core-alignment-plan.ko.md`](dotnet-bindings-core-alignment-plan.ko.md)
  메인 설계 authority 문서다.
  public surface, 고정 결정, 단계별 구현 범위, 검증 절차를 정의한다.
- [`dotnet-bindings-alignment-execution-guide.ko.md`](dotnet-bindings-alignment-execution-guide.ko.md)
  메인 플랜을 실제 실행 순서와 완료 판정 기준으로 고정한 실행 문서다.
- [`run_dotnet_bindings_alignment_execution.sh`](run_dotnet_bindings_alignment_execution.sh)
  bindings 작업 전용 랄프 루프 실행 래퍼다.
  내부적으로 [`core/tools/run_codex_execution_guide_loop.sh`](../../../../core/tools/run_codex_execution_guide_loop.sh)
  를 호출한다.
  실행 wrapper에는 별도 lock을 두지 않는다.
  병렬 실행이 필요하면 `--logs-dir`, `--gate-label`을 분리해서 사용한다.
  기본 로그 디렉터리는 [`logs/`](logs) 이다.

권장 사용 순서:

1. 메인 설계와 정책은
   [`dotnet-bindings-core-alignment-plan.ko.md`](dotnet-bindings-core-alignment-plan.ko.md)
   에서 확인한다.
2. 실제 착수 순서와 종료 판정은
   [`dotnet-bindings-alignment-execution-guide.ko.md`](dotnet-bindings-alignment-execution-guide.ko.md)
   에서 확인한다.
3. 자동 실행이 필요하면
   [`run_dotnet_bindings_alignment_execution.sh`](run_dotnet_bindings_alignment_execution.sh)
   를 사용한다.
4. 스크립트 smoke 점검은
   `./bindings/dotnet/plan/bindings/run_dotnet_bindings_alignment_execution.sh --max-iterations 0`
   로 수행한다. 정상 동작이면 종료 코드는 `0`이다.
