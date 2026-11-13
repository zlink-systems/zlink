from pathlib import Path
import json,statistics,subprocess
j=Path('/root/jobs/fx/cx/v21/s033/cdrain');p=j/'report.md';s=p.read_text()
rs=json.loads((j/'summary.json').read_text())
def med(pattern,variant,key='framework'):
 return statistics.median(r[key]['throughput'] for r in rs if r['pattern']==pattern and r['variant']==variant)
sha=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
s=s.replace(s.splitlines()[2],f'Status: C++ implementation committed locally as `{sha[:10]}`; 309 non-install tests and the official stream install consumer passed. All 30 final measurement cells completed with errors=0, abandoned=0 and no contaminated/drain-bound cells. No push. The full test completion criterion remains unmet because the existing Framework install-consumer fixture fix has not been authorized. Performance acceptance floors also remain unmet, as detailed below.')
a=s.index('## Decision ownership')
intro=f'''## Observed result

One gate-less hot Node reached **8 concurrent handlers** on the branch versus **1** on main in every PM run. Median hot throughput increased from **{med('pm','main'):,.1f} to {med('pm','branch'):,.1f} msg/s** ({med('pm','branch')/med('pm','main'):.2f}×). The requested concurrency, FIFO take, wake, permit, scope and shutdown checks passed.

Cheap-message throughput regressed: Framework request-backpressure decreased **{(1-med('request-backpressure','branch')/med('request-backpressure','main'))*100:.2f}%**, and send-saturation decreased **{(1-med('send-saturation','branch')/med('send-saturation','main'))*100:.2f}%**. Framework/binding ratios of 3-run medians changed **0.1973 → 0.1898** and **0.3006 → 0.2572**, respectively.

Spot latency increased under the achieved hot load: median-run p50 **0.300 → 1.355 ms**, p99 **0.502 → 7.586 ms**. The branch runs eight hot handlers concurrently while main runs one; these are the same offered workloads with different achieved CPU concurrency. This change demonstrates parallel progress and budget fairness, and is not a throughput or latency improvement for every workload.

'''
s=s[:a]+intro+s[a:]
s=s.replace('Stream connector passed. Framework consumer has a baseline typed-JSON DTO fixture error; scope approval pending for the concrete fix in `package-fixture-proposal.patch`.', 'Official stream consumer passed again against the final library. The official Framework consumer has a baseline typed-JSON DTO fixture error. A temporary copy with `package-fixture-proposal.patch` passed the complete install/build/run/symbol checks against the final library; repository fixture approval is pending (`package-final-validation.json`, `package-final-validation.log`).')
s=s.replace('The package fixture and public JSON headers are identical to frozen main. The proposed fixture-only migration to the existing typed JSON DTO contract was validated in its generated temporary consumer (build and execution passed); it is not applied to repository source without the pending scope decision.', 'The package fixture and public JSON headers are identical to frozen main. The proposed fixture-only migration to the existing typed JSON DTO contract passed in a temporary copy against the final library. The repository fixture remains unchanged. Package builds/configures and compiler-invoking checks use `cmake-slot.sh` / `cxx-slot.sh`, which forward to `cpp-build-slot.sh`; compile parallelism is at most 3. The installed build-slot wrapper takes the build command directly, so CMake builds use `cpp-build-slot.sh cmake --build … --parallel 3`. All tests/builds held measure.lock shared; measurements held it exclusively with `flock -o`.')
s=s.replace('The reported latency measures only', 'Per-run p50/p99 use nearest-rank percentiles; the summary takes the median of the three per-run values. The reported latency measures only')
a=s.index('Binding comparison:')
lines=['Aggregate Framework CPU and context switches (3-run medians; application CPU is the sum over its observed worker TIDs in each run). The per-thread tables above and `summary.json` retain individual values.','','| Cell | Revision | Application µs/msg | Receive µs/msg | Target switches/msg | Source switches/msg |','|---|---|---:|---:|---:|---:|']
for pattern in ('request-backpressure','send-saturation','pm'):
 for variant in ('main','branch'):
  selected=[r for r in rs if r['pattern']==pattern and r['variant']==variant]
  def cost(role):
   values=[]
   for r in selected:
    profile=r['framework']['profile'];threads=profile['target']['threads']
    values.append(sum(threads[t]['us_per_message'] for t in profile['role_threads'].get(role,[]) if t in threads))
   return statistics.median(values)
  target=statistics.median(r['framework']['profile']['target']['switches_per_message'] for r in selected)
  source=statistics.median(r['framework']['profile']['source']['switches_per_message'] for r in selected)
  lines.append(f'| {pattern} | {variant} | {cost("app-worker"):.3f} | {cost("receive"):.3f} | {target:.4f} | {source:.4f} |')
s=s[:a]+'\n'.join(lines)+'\n\n'+s[a:]
s+='''
## Remaining failures and unmet clauses

- **Official install-consumer test:** `framework/languages/cpp/tests/Zlink.Framework.PackageTests/install_consumer.cmake` still defines generated DTOs with legacy nlohmann converters instead of the current `zlink::json::dto_traits` contract. This is unchanged from frozen main. The concrete proposal is [package-fixture-proposal.patch](/root/jobs/fx/cx/v21/s033/cdrain/package-fixture-proposal.patch); its temporary copy passes all package checks against the final library. Applying it to repository source awaits the scope decision because [AGENTS.md:111](/root/worktree/zlink-1639c/AGENTS.md:111) says “unrelated failure를 임의로 고치거나 expectation을 낮추지 않는다.” The 309-test result excludes the two separately handled install tests; it is not an all-311-pass claim.
- **S/08 §7(c), throughput:** send-saturation 1K remains below the 0.90 Framework/binding floor: main 0.3006, branch 0.2572. The common bench §7.2 request-backpressure 0.80 floor is also unmet at 1K: main 0.1973, branch 0.1898. These gaps already exist on main and the branch regresses the measured ratios. No timeout, retry, HWM or budget was increased to change the verdict. Other payloads/patterns are outside this job's requested measurement set.
- **PM interpretation:** p99 increased as the hot owner used all eight application workers. The reported Spot interval is a same-host public request through the real application queue and gate; it does not measure remote Spot network latency. No numerical PM latency ceiling was specified, so the p50/p99 observations are reported without an invented pass threshold.
- No exception to the requested gate-less take, ready membership, waiter/wake, worker budget, permit, gated serialization, per-dispatch lifetime or shutdown clauses was required by this implementation. The listed C++ tests cover those behaviors. Cross-language implementations remain outside this explicitly C++ job.

Owner layer: C++ Framework mailbox and application executor.  
Spec authority: S/02 §2/§9; S/04 §3 step 5 and ready-owner conditions; S/08 I4/W1–W5/§7(f); API §11.  
Cross-language result: read-only comparison above; existing implementation differences remain outside the C++ scope.  
Change classification: **A — adaptation to the explicit contract in spec PR #3891**.

Changed repository files: scheduler/mailbox (`offload_executor.*`, `service_mailbox.*`), mesh and ClientServer drain integration (`mesh_node_host_service.*`, `channel_host_service.cpp`, `public_host_runtime.*`), CMake test registration, seven C++ test files and `tests/support/manual_internal_scheduler.hpp`. The working tree is clean. Commit: `5a585edc93`.

Validation artifacts: `build-production-node-storage.log`, `build-storage-focused.log`, `test-storage-focused.log`, `test-storage-focused-detail.log`, `build-storage-all.log`, `test-all-storage.log`, `test-all-storage.xml`, `format-final.log`, `build-post-commit.log`, `test-layout-post-commit.log`, `package-final-validation.log`, `versions.json`. Earlier diagnostic measurements are preserved in `measure-before-node-storage/` and `pm-before-independent-probes/`; none are included in the final tables.
'''
p.write_text(s)
