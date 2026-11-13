Model luna high: a small, fully specified test-helper change (Windows symbol resolution) with an exact target file.

# Task: #4143 — symbolize Windows allocation stacks in the Framework C++ allocation tests

Earlier briefs in other folders are history, not instructions for this task. Folder: JOB=/root/jobs/fx/cx/v21/s033/w4143b. Worktree: /root/worktree/zlink-v21-s033/w4143 on branch test/4143-windows-alloc-symbols (= origin/main). Background: /root/jobs/fx/cx/v21/s033/w4143/report.md (read it first).

## Goal
The win-x64 allocation-count tests print `exe+0xOFFSET` frames only. Make `describe_allocations` print function name and source file:line for each frame on Windows, so one hosted win-x64 run shows where the extra allocations in `RequestCallAllocations` (:222 local Actor registration 10 > 8, and the node request +1 checks at :411/:739) come from. No change to what the tests count or expect.

## Do
1. `framework/languages/cpp/tests/support/allocation_counter.cpp:58` `describe_allocations`: under `_WIN32`, resolve each frame with DbgHelp (`SymInitialize` once per process with `SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS`, `SymFromAddr`, `SymGetLineFromAddr64`); print `name+0xdisp file:line` after the existing module+offset text. DbgHelp is single-threaded: guard calls with a static mutex. Never allocate inside `record_allocation` (resolution only happens in `describe_allocations`, which runs outside the tracked window; keep it that way and say so in a comment). Link `dbghelp` for the test support target on Windows only. Keep Linux/macOS output unchanged.
2. PDB: the test executables must have PDBs next to them in the Windows Release build. Find where framework/languages/cpp/build-windows.ps1 and the Framework CMake set MSVC flags; add `/Zi` (compile) and `/DEBUG` `/OPT:REF` `/OPT:ICF` (link) for test targets only (keep optimisation /O2 so allocation behaviour matches Release). Do not change product library flags.
3. Prevention proposal (text only, in the report): a way for a PR that adds or changes an allocation-count expectation (files matching `*_allocations*.cpp`, `allocation_counter.*`, `*_cost.cpp`) to also get a win-x64 run before merge, e.g. a path-filtered win-x64 job in the PR checks or a label that dispatches framework-cpp win-x64. Cite the workflow lines (.github/workflows/framework-cpp.yml:283-284 says Windows runs only on schedule/manual/release). Do not edit workflows.
4. Only after /root/jobs/fx/cx/v21/s033/core124/verdict.md exists (a release gate is measuring; check with `test -e`, and until then do steps 1-3 and 5 only), build and run what you can on Linux to prove nothing changed there: build only `test_cpp_framework_request_call_allocations` (Release, through `/root/jobs/fx/cx/cpp-build-slot.sh` bare, no outer flock, 25 GB cap) and run it. The Windows code path cannot compile here; review it twice for MSVC correctness (types: `DWORD64`, `SYMBOL_INFO` buffer sizing, `IMAGEHLP_LINE64` SizeOfStruct).
5. clang-format the touched C++ files with the repository config (scripts/format/format.sh --check cpp or the .clang-format in framework/languages/cpp); git diff --check.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Do not take /root/jobs/fx/cx/measure.lock yourself in any form; only cpp-build-slot.sh bare.
- No git operations except diff and status; do not commit. No push, PR, GitHub operations, workflow dispatch. No spec edits. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not read other job folders except w4143.
- 리팩토링 점검: if the Windows and POSIX frame-printing branches duplicate logic, note it with file:line.
- Keep your own output short. Time box about 2 hours.

## Output: $JOB/report.md (Korean): changed files and why, the Windows code reviewed line by line, Linux build/run result with log path, prevention proposal. Save the diff as $JOB/change.diff.
