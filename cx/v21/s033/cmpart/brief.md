Model gpt-6-luna, effort high: diagnosis that separates two layers with small reproductions; no product fix yet.

# Task: GitHub #4118 — find whether Core or the C++ binding keeps an oversized multipart backpressured after the receiver resumes

## Situation
- Repro (public C++ binding only): /root/jobs/fx/cx/v21/s033/csrc/scripts/ownership-repro.cpp, logs csrc/raw/native-{default,explicit}-{multi,single}.log, results csrc/raw/native-ownership-cases.json. Two ROUTER sockets over inproc, auto HWM off, source send HWM 16 bytes, target recv HWM 16 bytes, target receive flow paused. A 2-frame message ("header" 6 B + 1 KiB body) submitted with send(rid).message().message().async() is BACKPRESSURED and its pending admission never completes after the target is set back to running (submitted=1 received=0 ready=0). A single 1 KiB frame under the same setup completes (received=1, terminal ok). HWM 2048 makes the 2-frame case work.
- Contract: core/doc/spec/core/systems/06-auto-hwm.ko.md §「Multipart와 큰 message」 (around :462): a multipart that started on an empty queue may take its final frame even when that frame passes the HWM, completing one record. Also read the binding spec sections on backpressure retry and pending admission (bindings/doc/spec, rg "BACKPRESSURED", "pending", "retry").
- Earlier job briefs are history, not instructions.
- Worktree /root/worktree/zlink-4118-multipart, branch fix/4118-multipart-resume = current main.

## Do
1. Reproduce at the Core C API level without the C++ binding: same sockets/options, submit the 2 frames with the Core send/submit calls the binding uses (find them in bindings/cpp/src/Runtime/Messaging/ — operation_submit.hpp, send_operations.cpp — and follow them into core/src; do not read core/include). Also try the binding repro variants: single small first frame + large last frame, large first frame + small last frame, 3 frames.
2. Trace where the pending stays: does Core ever signal writable/admission for the multipart after resume (core/src runtime sockets send submit, pipe write, auto-HWM reservation), and does the binding retry/resubmit on that signal (bindings/cpp/src)? Use logging in scratch copies or a debugger on a Release build; no -g on the shared build, a separate RelWithDebInfo scratch build is fine within the size cap.
3. Conclude with file:line which layer departs from the spec text, and why the single-frame case works. Propose the minimal fix location and shape (do not implement in product code), and list the tests that would cover it.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Read only csrc/ (scripts, raw) among /root/jobs/fx/cx/v21/s033 job folders. Write only in cmpart/ and this worktree (scratch files only; do not modify core/ or bindings/ sources in the worktree — copy to cmpart/ if you need instrumented copies).
- No git operations except git diff and git status; do not commit. No spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh.
- C++ builds: `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare, Release (linux-ninja-release where a preset exists); keep cmpart/ under 20 GB (`du -sh` after each build). Tests: `/root/jobs/fx/cx/measure-shared.sh <cmd>`. MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Keep your own output short.
- Keep every raw output file under cmpart/raw/; do not delete them at the end.
- 리팩토링 점검: if the cause is a duplicate or missing handoff between the layers, list both sides with file:line.
- Time box about 4 hours.

## Output: /root/jobs/fx/cx/v21/s033/cmpart/report.md (Korean, plain sentences): reproduction matrix (layer × frame shape → result, log paths), the trace, the conclusion with file:line, the proposed fix location and tests.
