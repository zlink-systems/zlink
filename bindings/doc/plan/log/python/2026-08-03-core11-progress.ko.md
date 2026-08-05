# Python binding Core 0.9.0 검증 log

작성일: 2026-08-03

이 log는 Python raw Core 0.9.0 작업에서 실행한 local candidate와 package evidence를 기록한다. 공통
`V11-R2`·`V11-M3-CORE-PKG` 승인 evidence가 현재 Core `11.2.0` candidate를 가리킨다는 뜻은 아니다.
이전 `11.1.0` evidence를 현재 candidate의 승인으로 사용하지 않았다.

## 현재 판정

Linux x86_64 local implementation gate는 통과했다. CPython 3.9 Docker와 host CPython 3.12에서 같은
candidate 절차를 각각 통과했다. 전체 상태는 `PARTIAL / NOT CLEAN`이며, 현재 Core candidate와 공통
V11 승인 evidence의 identity 확인 및 독립 frontier reviewer의 최종 `CLEAN` 판정이 남아 있다.

## 2026-08-04 current recheck

package recheck 당시 branch의 `HEAD`는 `1d724e5b3f2abbcde7b41a6143b6f6fbb947c588`였다. 당시
worktree를 기준으로 다시 만든 Core ledger candidate는 다음 identity를 갖는다.

```text
candidate: `.artifacts/v11/evidence/V11-M3-CORE-VERIFY/candidate-python-final-20260804.json`
candidateManifestSha256: 483df3ff20925fda60b3ac5a1c75e71e47c5eab871242623ee2c7fa66dd644bd
baseRevision: 1d724e5b3f2abbcde7b41a6143b6f6fbb947c588
aggregateSha256: 7f32732d7831728b62b9f3a1bb1d420b6f7c9f65952348e1eb4e76c7c27a855d
pathCount: 19
```

CPython 3.9과 3.12의 fresh package output은 이 Core revision과 runtime SHA를 기록한다.

```text
coreRevision: 1d724e5b3f2abbcde7b41a6143b6f6fbb947c588
coreRuntimeSha256: aff90818cc40df2ebeeb375489e147f7e23791bda28b0dac85bdc9462f59236e
coreCandidatePackageManifestSha256: 51959a43a84be11d30d98f11685bdc421562c6322b86b0e7b63596e5f4887033
pythonSourceManifestSha256: a5e4bb6551cb601fa67fa8851f0ab501f0016320eb0890dba51bdd93fb0d3fde
pythonSourceAggregateSha256: 399db451234ec87804f659e9535c211e59c97b18606a3c028c21bb96f7fa5c8c
cp39WheelSha256: 09517432ed295d8634534a43f0f487fe3ee947cfd3cbe8b00c4ac2311b1e1c9f
cp312WheelSha256: a28ab4d3e789addda5717b9f9a3117a16bf63d23aa66bc3653118e07f0c1fbf6
```

현재 package output은 source test `64 passed`, clean consumer와 installed sample `7/7`을 두 interpreter에서
통과했다. Single·multi perf smoke도 각각 `status=complete`와 `success=1, fail=0`을 반환했다.

기존 `.artifacts/v11/evidence/V11-R2/core-candidate-reply-match-completion-hwm-review-20260801.json`은
`d318525a4cf8496b6bef5d900c9a88330ea6d7e10ed4120ac0fd9f19d23f6765`만 승인한다. 현재 candidate에 이
evidence를 입력한 `scripts/local-package/core/verify-candidate.mjs` 결과는
`review evidence does not approve the supplied candidate manifest SHA-256`로 종료 코드 `1`이다.
따라서 fresh local package evidence는 확보했지만, 현재 candidate를 승인하는 독립 `V11-R2` evidence와
그 evidence를 참조하는 공통 `V11-M3-CORE-PKG` 결과는 아직 없다. 이 조건이 충족되기 전에는 전체 상태를
`CLEAN` 또는 완료로 올리지 않는다.

기존 승인 candidate를 현재 worktree에 재사용할 수 있는지도 read-only로 확인했다. 기존
`.artifacts/v11/evidence/V11-M3-CORE-VERIFY/candidate-reply-match-completion-hwm-20260801.json`과 그에
대응하는 V11-R2 evidence를 `verify-candidate.mjs`에 입력한 결과는
`candidate content drift: core/CMakeLists.txt`로 종료 코드 `1`이었다. 따라서 이전 candidate는 현재 Core
파일과도 일치하지 않으며, 현재 Python candidate의 승인으로 승격할 수 없다.

## Candidate identity

```text
sourceRevision: each `candidate-input.env` `CORE_REVISION`
coreManifest: .artifacts/wsl/bindings-candidate/core-11.2.0.env
coreManifestSha256: each `candidate-input.env` `CANDIDATE_MANIFEST_SHA256`
coreVersion: 11.2.0
coreRuntime: core/build/lib/libzlink.so.0.2.0
coreRuntimeSha256: each `candidate-input.env` `CORE_RUNTIME_SHA256`
coreSoname: libzlink.so.0
coreSymbolSha256: ac7b04ce8f3a8338b82328ca03d6e93892f56ae57bb78569f9901ba5f65d5823
coreSourceSha256: 9888dd12f90930fb88a9b57b632f06bf44b3c05c6229246ad4cd62d8c21de1ce
coreHeaderSha256: f8d51ae49c3c3bb7d2ea54d1d6f067af47de37922dc93ef4e2cc8a624345a5a9
coreSpecSha256: f89f006c105048acaf5bdfcb2ce252995bc72ded9b0a8e7354813a150dfc43b1
```

현재 Core worktree에서 공통 review를 요청할 후보도 별도로 만들었다. 이는 package 승인 evidence가
아니며, 현재 변경 경로를 고정해 reviewer가 동일한 입력을 확인하기 위한 draft다.

```text
coreLedgerCandidate: `.artifacts/v11/evidence/V11-M3-CORE-VERIFY/candidate-python-final-20260804.json`
coreLedgerCandidateBaseRevision: candidate JSON `baseRevision`
coreLedgerCandidateManifestSha256: SHA-256 of the candidate JSON
coreLedgerCandidateAggregateSha256: candidate JSON `aggregateSha256`
coreLedgerCandidatePathCount: candidate JSON `pathCount`
```

기존 `V11-R2` review evidence
(`.artifacts/v11/evidence/V11-R2/core-candidate-reply-match-completion-hwm-review-20260801.json`,
SHA-256 `171a9cc8f7203500de08050dcb74ecd36b4c9ce55a75a14ce1bee283705c9e04`)는 candidate SHA-256
`d318525a4cf8496b6bef5d900c9a88330ea6d7e10ed4120ac0fd9f19d23f6765`만 승인한다. 이 evidence를 현재
candidate에 입력해 `verify-candidate.mjs`를 실행한 결과는 의도대로 `review evidence does not approve
the supplied candidate manifest SHA-256`로 실패했다. 따라서 현재 candidate에 대한 독립 `V11-R2` review와
그 결과를 사용하는 `V11-M3-CORE-PKG` evidence가 아직 필요하다.

## Python source and wheel evidence

```text
sourceManifest: `.artifacts/wsl/bindings-candidate/python-source-manifest-11.2.0.json` 및 각 output root의 동일 manifest
sourceManifestSha256: 각 `candidate-input.env`의 `PYTHON_SOURCE_MANIFEST_SHA256`
sourceAggregateSha256: 각 `candidate-input.env`의 `PYTHON_SOURCE_AGGREGATE_SHA256`
candidateInput: `.artifacts/wsl/bindings-candidate/python39/python/candidate-input.env` and the corresponding
`python312/python/candidate-input.env`
wheel: `python39/python/wheels/zlink-11.2.0-cp39-cp39-linux_x86_64.whl` and
`python312/python/wheels/zlink-11.2.0-cp312-cp312-linux_x86_64.whl`
wheelSha256: 각 output root의 `python/SHA256SUMS` wheel entry
packagedNativePayloadSha256: 각 `candidate-input.env`의 `PACKAGED_NATIVE_PAYLOAD_SHA256`
```

`candidate-input.env`의 Core manifest SHA, runtime SHA, source manifest SHA와 aggregate SHA는 위 값과
일치한다. Source manifest SHA와 aggregate SHA는 각 `candidate-input.env`의
`PYTHON_SOURCE_MANIFEST_SHA256`·`PYTHON_SOURCE_AGGREGATE_SHA256`를 사용한다. Wheel SHA는 각 output
root의 `python/SHA256SUMS` wheel entry를 사용한다. Wheel에는 `py.typed`와
`linux-x86_64/libzlink.so.0.2.0`만 포함되며 `libzlink_c`, 이전 SONAME, cross-platform payload와 source
path는 포함되지 않는다.

## 실행 결과

### Source test와 static check

```bash
ZLINK_LIBRARY_PATH="$PWD/core/build/lib/libzlink.so.0.2.0" \
  PYTHONPATH=bindings/python/src PYTHONDONTWRITEBYTECODE=1 \
  pytest -q bindings/python/tests
```

결과: source extension을 선택한 interpreter로 먼저 build한 뒤 `64 passed`.

```bash
PYTHONPATH=bindings/python/src python3 -m py_compile \
  $(rg --files bindings/python/src/zlink -g '*.py')
```

결과: 종료 코드 `0`.

`npx --yes pyright@1.1.411 --project bindings/python/pyrightconfig.json` 결과는 `0 errors, 0 warnings,
0 informations`이다. 설정 target은 Python `3.9`이고 대상은 public `src/zlink/contracts` tree다.

### Candidate package와 clean consumer

```bash
scripts/local-package/bindings-candidate/create-manifest.sh \
  .artifacts/wsl/bindings-candidate/core-11.2.0.env
scripts/local-package/bindings-candidate/build-wsl.sh \
  --language python \
  --manifest .artifacts/wsl/bindings-candidate/core-11.2.0.env \
  --package-version 11.2.0 \
  --python-executable python3.12

# CPython 3.9 Docker에서도 같은 command에 --python-executable python을 지정한다.
```

결과: 각 output root의 `candidate-input.env`가 가리키는 동일 Core candidate를 기준으로 CPython 3.9
Docker와 host CPython 3.12 모두 종료 코드 `0`. 각 source test는 `64 passed`, clean wheel
consumer는 Pair roundtrip과 wheel payload load-map 확인을 통과했다.
Builder는 같은 Core prefix와 source manifest를 사용해
각 interpreter용 wheel을 만들고, 새 venv에서 source checkout과 `core/build` fallback 없이
`zlink.version()`과 Pair message roundtrip을 실행했다. `/proc/self/maps`는 각 venv의 wheel payload를
가리켰다. 같은 venv에서 `run_samples.py --installed`를 실행한 결과도 각각 `7/7`이다.

Source runner 결과는 다음 7개 process가 모두 통과했다.

```text
dealer-router/request-reply/callback
pair/recv
dealer-router/recv
pubsub/recv
stream/recv
stream/packet-callback
monitor/recv
```

### Perf smoke

Single runner:

```bash
ZLINK_LIBRARY_PATH="$PWD/core/build/lib/libzlink.so.0.2.0" \
  LD_LIBRARY_PATH="$PWD/bindings/python/src/zlink/native/linux-x86_64" \
  PYTHONPATH=bindings/python/src \
  bindings/python/perf/run_benchmarks.sh --smoke --pattern PAIR \
  --duration 1 --msg-sizes 64 --transports inproc --runs 1
```

결과: `status=complete`, `actual_result_lines=5`, runtime SHA는
`aff90818cc40df2ebeeb375489e147f7e23791bda28b0dac85bdc9462f59236e`이다.

Multi runner:

```bash
ZLINK_LIBRARY_PATH="$PWD/core/build/lib/libzlink.so.0.2.0" \
  LD_LIBRARY_PATH="$PWD/bindings/python/src/zlink/native/linux-x86_64" \
  PYTHONPATH=bindings/python/src \
  bindings/python/perf/run_benchmarks_multi.sh --smoke \
  --pattern DEALER_ROUTER --duration 1 --msg-sizes 64 --transports tcp \
  --runs 1 --clients 1
```

결과: `status=complete`, `success=1`, `fail=0`, `actual_result_lines=5`, runtime SHA는
`aff90818cc40df2ebeeb375489e147f7e23791bda28b0dac85bdc9462f59236e`이다.
두 smoke 실행은 공식 성능 report를 만들지 않았다.

## POSD·DDD와 남은 조건

`2026-08-03-posd-ddd-review.ko.md`에서 Context, Message/Received, Socket, callback dispatcher와
package adapter를 lifecycle·ownership owner로 분리했다. 초기 독립 review가 확인한 ChannelName,
close retry, input error, enum mapping과 dead example 문제를 owning layer에서 수정했다. Cost inventory는
allocation, copy, lock, GIL과 no-cost를 모두 분류했고 `unclassified=0`이다. 수정 후 source test는
`64 passed`이며, 같은 최종 manifest를 읽은 독립 re-review는 아직 필요하다.

이는 Codex self-review이므로 다음 조건이 남는다.

1. 공통 담당자가 현재 `11.2.0` candidate와 독립 V11-R2·V11-M3-CORE-PKG evidence의 identity를 확인한다.
2. 독립 frontier reviewer가 같은 source manifest와 fresh evidence를 읽고 최종 `CLEAN`을 판정한다.

Linux aarch64, macOS와 Windows는 현재 release target이 아니므로 별도 candidate와 native consumer
검증을 완료하기 전에는 지원 범위에 넣지 않는다.

## 2026-08-04 Codex self-review refresh

package candidate snapshot의 HEAD는 `de948ac89ec753cfda5b1b1f9869c78336f647da`이며, 이후 checkout에는
이 progress log를 기록한 문서 commit만 추가됐다. Core 0.9.0.2.0 runtime SHA는
`ce28d7908bf62a1b39b481aad2a76c6e76955e3a93ea73e1cbdaa913c4883138`이다. Core 변경 19개를
정식 spec·계획 문서와 대조하고 `lb_t`의 smooth weighted round-robin, routing ID tie-break, candidate
변경, write failure recovery와 `0..10000` validation을 확인했다. 현재 `test_router_multiple_dealers`는
`17 Tests 0 Failures`, `unittest_typed_option`은 `2 Tests 0 Failures`다.

Self-review에서 `Message.try_copy_to()`의 contract와 구현이 어긋난 사실도 확인했다. contract는 복사한
byte 수 또는 capacity 부족 시 `None`을 요구했지만 구현은 `True/False`를 반환했다. owner layer 구현을
수정하고 writable destination, capacity 부족, invalid readonly destination을 contract test로 고정했다.
수정 결과 Python source test는 `65 passed`, `pyright`는 `0 errors, 0 warnings, 0 informations`다.

새 local package identity는 다음과 같다.

```text
coreManifest: .artifacts/wsl/bindings-candidate/core-11.2.0-python-self-20260804-v3.env
coreManifestSha256: 53aef79e4ae5ebe0e525d0ada57e5aaac16316f25ad89f1af6f1c8ebdb6f9d9a
coreRevision: de948ac89ec753cfda5b1b1f9869c78336f647da
candidateInputSha256: 53aef79e4ae5ebe0e525d0ada57e5aaac16316f25ad89f1af6f1c8ebdb6f9d9a
pythonCandidate: .artifacts/v11/evidence/V11-M3-CORE-VERIFY/candidate-python-self-20260804.json
pythonCandidateSha256: 5364792d464c56e04c33477dac096f09daa6e5f9ddc7499b5bbd4acb1b5bb156
pythonCandidateAggregateSha256: 7f32732d7831728b62b9f3a1bb1d420b6f7c9f65952348e1eb4e76c7c27a855d
pythonSourceManifestSha256: 8e6d73b48ab6a26da195e3863240fc1720bca9c5134300d0ea341a0e1e6b2441
pythonSourceAggregateSha256: 4d1e014e03e56d85e732fa165aaab6e4823a79721cd72c4bd26f78f87eb54cfa
cp39WheelSha256: b5d2b84962d18bdd90e4922bbf64104166fd9fb4940e8c9e144279133b6ed8d3
cp312WheelSha256: 74cc57d59c45292eeb763e135c94baa2a543b99b4a2cc5c55507d1a95010b033
```

CPython 3.9은 Ubuntu 24.04 + deadsnakes CPython 3.9.25 환경에서, CPython 3.12는 host 3.12.3에서
같은 Core candidate를 사용했다. 두 package의 source test는 각각 `65 passed`, clean consumer와
installed sample은 각각 `7/7`이다. 현재 runtime으로 다시 실행한 single perf smoke는
`status=complete`, `actual_result_lines=5`, multi perf smoke는 `status=complete`, `success=1`,
`fail=0`, `actual_result_lines=5`다.

이 refresh는 Codex self-review와 local package evidence를 갱신한 것이다. 기존 V11-R2 evidence는 이전
candidate만 승인하므로 현재 candidate에 입력하면 `review evidence does not approve the supplied
candidate manifest SHA-256`로 실패한다. 따라서 독립 V11-R2·V11-M3-CORE-PKG evidence와 frontier reviewer의
최종 `CLEAN` 판정은 여전히 남아 있다.

## 2026-08-04 Codex self-review package refresh (v5)

이전 v4 manifest를 만든 직후 별도 성능 작업의 문서 commit이 추가되어 CPython 3.9 build가
`Core revision drift`로 중단됐다. 이 오류는 manifest가 가리키는 checkout과 실제 build checkout이
다를 때 package 생성을 막는 provenance 검사 결과다. 현재 package snapshot은 그 문서 commit까지
포함한 `0e2b9a8f82dd5e365a52a7381189d1b48b3b2ccd`로 다시 고정했다. 이후에는 이 progress log만
기록하는 commit을 추가하며 package snapshot과 섞지 않는다.

```text
coreManifest: .artifacts/wsl/bindings-candidate/core-11.2.0-python-self-20260804-v5.env
coreManifestSha256: 287967f72798bd18764baaa9b525905b8264a3372e4cccdcb8d4e07ff5b3e240
coreRevision: 0e2b9a8f82dd5e365a52a7381189d1b48b3b2ccd
coreRuntimeSha256: ce28d7908bf62a1b39b481aad2a76c6e76955e3a93ea73e1cbdaa913c4883138
cp312CandidateInputSha256: 41d863ebe74a81c34c9aec1ab415acfa1e08bec8aff65d25b80b7b576fbe248f
cp39CandidateInputSha256: d7c8f24b92d626b1308320066ce07b7d4054f3d1320de1ffade1f57b588b40c1
pythonSourceManifestSha256: 42b0f105dfb9ca94be0678758a7028f014826148d18d2b1cde4fcdc8edca71d1
pythonSourceAggregateSha256: 4d1e014e03e56d85e732fa165aaab6e4823a79721cd72c4bd26f78f87eb54cfa
cp312WheelSha256: 339488cd49f96cbea1a9bf4f8750a663337be2b9eaa9b0fb569a1075d5a039c0
cp39WheelSha256: 8f176ab2ee93c9cccddb5319487cdf6402fab8e3e54954a904a2e93d8ac61354
```

v5 package는 CPython 3.12.3과 Ubuntu 24.04 + deadsnakes CPython 3.9.25에서 각각 source test
`65 passed`, clean consumer, installed sample `7/7`을 통과했다. 같은 runtime으로 실행한 perf smoke도
single은 `status=complete`, `actual_result_lines=5`, multi는 `status=complete`, `success=1`,
`fail=0`, `actual_result_lines=5`를 반환했다. Candidate JSON은 현재 Core owned path와 aggregate를
검증했지만, 기존 V11-R2 evidence는 이 candidate manifest를 승인하지 않아 독립 review와 최종
`CLEAN` 판정이 필요한 상태다.

## 2026-08-04 Codex self-review package refresh (v6 final)

계획서에서 snapshot 번호와 output directory를 직접 참조하지 않도록 정리한 commit까지 포함해
최종 package snapshot을 다시 생성했다. 이 변경 뒤에는 계획서를 수정하지 않으며, 아래 snapshot은
이 progress log만 추가하기 전의 checkout을 가리킨다.

```text
coreManifest: .artifacts/wsl/bindings-candidate/core-11.2.0-python-self-20260804-v6.env
coreManifestSha256: 7994cf0f933cbf276e0fa0662ed5c0d6b6362985e538ae5ae7c47905fbac3310
coreRevision: fcbf9e2a2b04e731a547a6ceb39aed3f7e35bb89
coreRuntimeSha256: ce28d7908bf62a1b39b481aad2a76c6e76955e3a93ea73e1cbdaa913c4883138
cp312CandidateInputSha256: 4a248e11ecc410271b43641495d9231a94801467c3f3fed3efc745bbf91665bc
cp39CandidateInputSha256: f3aaebbb5abf680ead367800e85ea55cee57a6c3499620c05aa1c28454924445
pythonSourceManifestSha256: 38d300410aba10c22ea16040465f9d8a2297f3e1efe7270ccc23200fd8ae42c5
pythonSourceAggregateSha256: 4d1e014e03e56d85e732fa165aaab6e4823a79721cd72c4bd26f78f87eb54cfa
cp312WheelSha256: 9b6660f1c2b82c96baf8207d4eba16a4e4a6496a0058a2747154df02b3a367b6
cp39WheelSha256: a8df7d5d463455dee5cf4ef0b08dbf5a64614f9b3d987ce86e0e33da4e136f98
```

v6 package는 CPython 3.12.3과 Ubuntu 24.04 + deadsnakes CPython 3.9.25에서 각각 source test
`65 passed`, clean consumer, installed sample `7/7`을 통과했다. Core runtime SHA는 v5와 같으며,
single·multi perf smoke도 같은 runtime에서 완료됐다. 기존 V11-R2 evidence는 여전히 이전 candidate만
승인하므로 현재 candidate에 입력하면 `review evidence does not approve the supplied candidate
manifest SHA-256`로 실패한다. 따라서 독립 V11-R2·V11-M3-CORE-PKG evidence와 frontier reviewer의
최종 `CLEAN` 판정은 이 자체 검토 범위 밖의 남은 gate다.

## 2026-08-04 CLEAN gate pursuit audit

현재 Python 작업이 요청할 수 있는 Core candidate는 다음 identity로 현재 파일과 다시 대조했다.

```text
candidate: .artifacts/v11/evidence/V11-M3-CORE-VERIFY/candidate-python-final-20260804.json
candidateManifestSha256: 483df3ff20925fda60b3ac5a1c75e71e47c5eab871242623ee2c7fa66dd644bd
baseRevision: 1d724e5b3f2abbcde7b41a6143b6f6fbb947c588
aggregateSha256: 7f32732d7831728b62b9f3a1bb1d420b6f7c9f65952348e1eb4e76c7c27a855d
pathCount: 19
```

`verify-candidate.mjs`는 이 후보의 current content, base hash, direct input, Core changed-path coverage와
aggregate를 통과한 뒤 review approval 단계에서만 중단됐다. 현재 저장소의 표준
`.artifacts/v11/evidence/V11-R2/result.json`은 다른 후보 SHA `06d72dab...`를 기록하고, Python 작업에
사용했던 `core-candidate-reply-match-completion-hwm-review-20260801.json`은
`d318525a...`만 승인한다. 둘 다 현재 후보의 승인으로 사용할 수 없다.

기존 independent `CLEAN` reviewer evidence가 승인한 `f4897bb...` 후보도 read-only로 확인했다. 해당
후보를 현재 checkout에 입력하면 `candidate content drift: VERSION`으로 실패하므로 현재 Python 후보에
승격하지 않았다. 따라서 남은 입력은 현재 후보 SHA `483df3ff...`를 `details.approvedCandidateManifestSha256`
로 기록한 새로운 표준 V11-R2 passed evidence이며, 그 뒤에야 동일 SHA를 사용하는
V11-M3-CORE-PKG 결과와 frontier reviewer의 최종 `CLEAN` 판정을 생성할 수 있다. 이 evidence를
구현자가 임의로 생성하면 independent gate의 의미가 사라지므로 자체 검토 결과로 대체하지 않는다.

## 2026-08-04 Codex self-review package refresh (v7)

직접 검토에서 `NativePoller.close()`가 native destroy 결과를 확인하기 전에 `_handle`을 `None`으로
바꾸는 lifecycle 오류를 확인했다. destroy가 실패하면 호출자는 같은 native owner를 다시 close할 수
없었다. 다른 native owner와 같은 성공 후 상태 전환으로 수정하고, 실패 뒤 재시도하는 회귀 테스트를
추가했다. 수정 commit은 `092f1c71cf7`이며, 이 commit 뒤 현재 checkout에서 package를 다시 만들었다.

```text
coreManifest: .artifacts/wsl/bindings-candidate/core-11.2.0-python-self-20260804-v7.env
coreManifestSha256: 91bada5c3da2259e1473e82bd04800a3bec94f92e81b689b8149fcf12a3b3d5d
coreRevision: 6c7fbf9c84e98591124b73f5c2c907710524f870
coreRuntimeSha256: ce28d7908bf62a1b39b481aad2a76c6e76955e3a93ea73e1cbdaa913c4883138
cp312CandidateInputSha256: e4aeeb116ea28d012d647027a7ee1644dd799c0bb215b1bfba70a81b20adb923
cp39CandidateInputSha256: 1d9170a17900796f74527f3557a1305b31d73cfe5a71df0450283b50cb4725dc
pythonSourceManifestSha256: 7702652eedfa16291f0cf3fb1b4dddd0944f81e009407de60e3e749a1e45fa5e
pythonSourceAggregateSha256: 95ea5b19d4df7227ed1ddedfd47f5004379e152f8be24e1b99beb0d82a662154
cp312WheelSha256: 8ff24d48137b8cd1fe3c5b5932c12c1d3951f8e208b85249ee837bdcaca5bb37
cp39WheelSha256: 40050adb43cb0620a9effc0288dd92fdfc4f77b3b77b84138712a5fa92a35264
```

CPython 3.12.3과 Ubuntu 24.04 + deadsnakes CPython 3.9.25에서 각각 source test는 `66 passed`,
clean wheel consumer와 installed sample은 각각 `7/7`을 통과했다. Pyright도 `0 errors, 0 warnings,
0 informations`를 반환했다. 동일한 Core runtime으로 실행한 v7 perf smoke는 single이
`status=complete`, `actual_result_lines=5`, multi가 `status=complete`, `success=1`, `fail=0`,
`actual_result_lines=5`를 반환했다.

이 결과는 수정된 Python binding에 대한 Codex self-review PASS와 local package evidence다. 현재
V11-R2 evidence가 v7의 Core manifest identity를 승인했다는 뜻은 아니다. 별도 reviewer가 같은
candidate identity를 승인하고 V11-M3-CORE-PKG와 frontier `CLEAN` 판정을 생성하기 전까지 formal
independent gate는 `PARTIAL`로 유지한다.

## 2026-08-04 Codex self-review package refresh (v8)

계획서의 contract test 수와 poller lifecycle 수정 내용을 package direct input에 반영하기 위해 v8을
다시 생성했다. Python 구현과 Core runtime은 v7과 같고, source manifest는 갱신된 계획서 hash를
포함한다.

```text
coreManifest: .artifacts/wsl/bindings-candidate/core-11.2.0-python-self-20260804-v8.env
coreManifestSha256: b8096355c6a2a38b5a7042df014f4b1dde1367cce9a0aee85db9b8dc2a722ee9
coreRevision: b17fc65f2e98d76604fed3bb508f74634ea7e48f
coreRuntimeSha256: ce28d7908bf62a1b39b481aad2a76c6e76955e3a93ea73e1cbdaa913c4883138
cp312CandidateInputSha256: 15e0ca0d240bfc9f6d077f077cb4475d8581edadcdb6e04c88cbb05acea18e88
cp39CandidateInputSha256: d779a8260f19b908adf837bfb3f736a0ecf1090e8024e070f8fccdd846286033
pythonSourceManifestSha256: 229d0e3f801cc70ed4318d0581e7444a7e498f5e16e26bf321257377cd472e67
pythonSourceAggregateSha256: 95ea5b19d4df7227ed1ddedfd47f5004379e152f8be24e1b99beb0d82a662154
cp312WheelSha256: d056521fea1466af14070961cf0b5b7f6f25209045ce5a5ee638e09815e62616
cp39WheelSha256: bf71e0231034d9861c6076af32bf5ad77b290503414fabb0ae96dcb1faf977e6
```

CPython 3.12.3과 Ubuntu 24.04 + deadsnakes CPython 3.9.25에서 각각 source test는 `66 passed`,
clean wheel consumer와 installed sample은 각각 `7/7`을 통과했다. v8 perf smoke도 single
`status=complete`, `actual_result_lines=5`, multi `status=complete`, `success=1`, `fail=0`,
`actual_result_lines=5`로 확인했다. Pyright 결과는 `0 errors, 0 warnings, 0 informations`다.

v8도 local package와 Codex self-review 증거이며 formal independent approval이 아니다. 별도 reviewer가
동일 candidate identity를 승인하고 V11-M3-CORE-PKG와 frontier `CLEAN` 판정을 생성하기 전까지
공통 ledger 상태는 `PARTIAL`로 유지한다.

## 2026-08-04 independent Codex agent review

별도 Codex reviewer agent `019fc8e9-ba7e-7bc1-8b73-b297621863aa`에 현재 Python 변경 범위의
read-only review를 요청했다. reviewer는 `PASS`를 반환했고 actionable correctness, contract, ownership,
provenance finding은 보고하지 않았다.

reviewer가 확인한 핵심 근거는 `poller.py:143-150`의 destroy 성공 후 `_handle` 해제 순서,
`test_lifecycle_contract.py:181-194`의 destroy 실패 후 재시도 ownership, Core
`poller_api.cpp:40-54`의 `void **` destroy semantics 일치다. reviewer 실행 결과는 Python `66 passed`,
manifest aggregate와 322개 파일 hash, wheel hash, `unzip -t`, `git diff --check` PASS였다. Pyright는
reviewer 환경에 실행 파일이 없어 재실행하지 못했지만, v8 package verification에서 `0 errors, 0 warnings,
0 informations`를 별도로 확인했다.

이 결과는 구현 agent와 독립적인 Codex peer-agent review PASS다. formal V11-R2/ V11-M3-CORE-PKG
evidence와 frontier `CLEAN` ledger gate를 대체하지 않으므로, 해당 공통 gate는 별도 승인 전까지
`PARTIAL`로 유지한다.
