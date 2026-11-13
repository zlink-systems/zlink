#!/usr/bin/env python3
import json
import pathlib
import statistics
import subprocess
import xml.etree.ElementTree as ET

job = pathlib.Path(__file__).parent
before = json.loads((job / 'before.json').read_text())
after = json.loads((job / 'after-final.json').read_text())
tree = '/root/worktree/zlink-3867a'
sha = subprocess.check_output(['git', '-C', tree, 'rev-parse', 'HEAD'], text=True).strip()

sites = [
    ('request_task_access_t::receive', 'request_task_access_t::receive<', 'Admission source and state share one allocation; bind receivers directly to existing task states.'),
    ('submit_request_reply_message_erased', 'route_client_t::submit_request_reply_message_erased(', 'Coallocate target/decoder/projection; remove unused default media string and borrow the retained selected serializer media string during synchronous encoding.'),
    ('request_receiver_t copy/constructor', '::request_receiver_t(', 'Copy shared callable ownership instead of cloning nested std::function captures.'),
    ('task_shared_state_t completion observer', ('::on_completed', '::on_terminal'), 'Coallocate callback and registration; store terminal callback in its list node.'),
    ('observe_task_terminal', 'detail::observe_task_terminal<', 'Forward the typed callback without a second std::function box; see relocated terminal-node attribution below.'),
    ('observe_task_completion', 'detail::observe_task_completion<', 'Forward the typed callback into the combined observer allocation.'),
    ('channel.hpp request_to_node', 'route_client_t::request_to_node<', 'Own the request directly in the submit closure; share that closure.'),
    ('raw_mesh_node_owner request_with_header', 'raw_mesh_node_owner_t::request_with_header(', 'Reuse the existing node-owned outer wire vector capacity; clear all contents at each return/throw. Inline the bounded raw callback.'),
    ('raw_route_port request', 'raw_route_port_t::request(', 'Borrow target bytes and consume the wire vector synchronously by rvalue reference. See inlining attribution below.'),
    ('operation_registry register_operation', 'operation_registry_state_t::register_operation(', 'Unchanged: pending completion identity and deadline ownership remain in the registry.'),
    ('classify_node_target_from_topology', 'mesh_node_runtime_t::classify_node_target_from_topology(', 'Transparent span lookup avoids a temporary routing-id byte vector.'),
    ('envelope_codec encode_header', 'envelope_codec_t::encode_header(', 'Keep the final encoded envelope allocation; borrow the selected media type through the existing private codec overload.'),
    ('client_call_codec create_envelope', 'client_call_codec_t::create_envelope(', 'Unchanged envelope representation; avoid the separate media-type copy in the caller.'),
    ('public_host_runtime request_to_node', 'public_host_runtime_t::request_to_node(', 'Borrow submit bytes; retain the fixed-size routing_id_t in the pending classifier.'),
    ('encode_node_request_header', 'protocol::encode_node_request_header(', 'Reserve the complete 13-byte header once, then write the identical prefix and sequence.'),
    ('materialize_binding_part (unchanged, attribution control)', 'backend::materialize_binding_part(', 'Out of scope and unchanged. Some of its allocations inline into raw_route_port request; compare their combined count.'),
    ('relocated admission terminal node', '::end_at_admission_failure()', 'New first-frame attribution for the remaining terminal-observer node, not a new control decision.'),
]

def buckets(data):
    result = [0.0] * len(sites)
    for row in data['submitter']['rows']:
        for i, (_, patterns, _) in enumerate(sites):
            if isinstance(patterns, str):
                patterns = (patterns,)
            if any(p in row['site'] for p in patterns):
                result[i] += row['calls_per_msg']
                break
    return result

b, a = buckets(before), buckets(after)
lines = [
    '# C++ request submit allocations — issue #3867', '',
    f'Commit: `{sha}` on `perf/3867-submit-alloc`. Committed locally; not pushed.', '',
    f"The submitter measured **{before['submitter']['calls_per_message']:.3f} → {after['submitter']['calls_per_message']:.3f} heap calls/message** "
    f"({(1-after['submitter']['calls_per_message']/before['submitter']['calls_per_message'])*100:.2f}% fewer), against the ≤30 goal. "
    'Public and physical reply states remain separate. The measured final source changes representation, not submission/completion policy.', '',
    '## Allocation census', '',
    'Fresh reference: `/root/worktree/zlink-ratio040`, main `ee11390257`, prebuilt Release. Changed tree started at `3fb411b4ab`. '
    'The user-specified reference and starting base differ in offload_executor.cpp and state_lane.cpp/.hpp; throughput ratios compare those complete builds rather than an isolated same-base patch. Both use Core/binding 1.23.0. Framework only, request-backpressure, 1024 bytes, 10-second warmup and 20-second active phase; an approximately 8-second capture runs inside active. '
    'Reused `calloc` alloc_trace.so, collect.py and instrumented runner through `census.sh`. Each run held `flock -o /root/jobs/fx/cx/measure.lock`. '
    'The final runner checked that Ninja had no pending benchmark work.', '',
    'Exact per-thread/process malloc/calloc/realloc counters use the completed-message delta from the same capture window. '
    'Site shares are sampled at 1/256 and rescaled to those exact totals. Aligned allocation APIs and direct mmap are outside this existing counter. '
    'No allocator replacement or custom allocation policy was added to the Framework.', '',
    '| Scope | Before calls/message | After calls/message |', '|---|---:|---:|',
]
for key, label in [('submitter', 'Source submitter'), ('source', 'Whole source process'), ('target', 'Whole target process')]:
    lines.append(f"| {label} | {before[key]['calls_per_message']:.3f} | {after[key]['calls_per_message']:.3f} |")
lines.append(f"| Source + target processes | {sum(before[k]['calls_per_message'] for k in ['source','target']):.3f} | {sum(after[k]['calls_per_message'] for k in ['source','target']):.3f} |")
lines += ['', '| Capture | Before | After |', '|---|---:|---:|',
          f"| Completed messages in capture | {before['submitter']['messages']:,} | {after['submitter']['messages']:,} |",
          f"| Submitter sampled stacks | {before['submitter']['samples']:,} | {after['submitter']['samples']:,} |", '',
          '### Per-site submitter attribution', '',
          'Same first Framework frame convention as the original census. A reported 0.000 means no sampled allocation was attributed to that site; '
          'it does not imply that every allocation in an inlined callee disappeared. The independent receiver-copy probe verifies its specific zero-allocation claim directly.', '',
          '| Allocating site | Before calls/message | After calls/message | Representation change |', '|---|---:|---:|---|']
for i, (label, _, change) in enumerate(sites):
    lines.append(f'| `{label}` | {b[i]:.3f} | {a[i]:.3f} | {change} |')
lines += ['',
          'Inlining moves some unchanged materialization allocations from `materialize_binding_part` into `raw_route_port_t::request`. '
          'Read those two rows together; the sum drops by approximately one callback allocation. '
          'The receiver/submit closure and terminal-node rows also move attribution when allocations are combined or inlined. '
          'The exact total, rather than a sum of assumed object counts, determines goal attainment. '
          '`encode_application_payload`, `application_payload_t`, `message_parts_t` and `raw_binding_adapter.hpp` were not modified. '
          'Only `encode_node_request_header` changed in service_wire_codec.cpp.', '',
          'Raw evidence: `before.json`, `after-final.json`, `census-before/`, `census-final/`, their runner logs, and the reused `analyze.py`. '
          'Intermediate captures (35.308, 31.303 and 29.240 before comparator consolidation) preceded the final source and are not the final result. The interrupted 20-cell preliminary A/B is under ab-before-comparator/ and is excluded from the final table.', '',
          '## Throughput A/B', '',
          'Framework only, 1024 bytes, WARMUP_SECONDS=30, DURATION_SECONDS=5, RUNS=1 per cell. '
          'Four rounds; each round runs modified then reference for request-backpressure, request-serial and send-saturation. '
          'Each individual cell holds the exclusive measurement lock. The C++ condition-B pin patch fixes source to CPUs 0–7 and target to 8–15 on both trees. '
          'The temporary patch was removed from this worktree before commit. No payload/wire optimization from the separate job is included.', '',
          '| Pattern | Round | Reference msg/s | Modified msg/s | Change |', '|---|---:|---:|---:|---:|']
all_cells = []
for pattern in ['request-backpressure', 'request-serial', 'send-saturation']:
    values = {'before': [], 'after': []}
    for n in range(1, 5):
        row = {}
        for variant in ['before', 'after']:
            p = job / 'ab' / str(n) / pattern / variant / 'run1' / f'zlink-framework-cpp-{pattern}-1024' / 'results.json'
            cell = json.loads(p.read_text())['cells'][0]
            all_cells.append((str(p), cell))
            row[variant] = cell['throughput_per_second']
            values[variant].append(row[variant])
        lines.append(f"| {pattern} | {n} | {row['before']:,.2f} | {row['after']:,.2f} | {(row['after']/row['before']-1)*100:+.2f}% |")
    bm, am = (statistics.median(values[k]) for k in ['before', 'after'])
    lines.append(f'| {pattern} | **Median** | **{bm:,.2f}** | **{am:,.2f}** | **{(am/bm-1)*100:+.2f}%** |')
bad = []
for p, c in all_cells:
    for k in ['errors','server_rejected_count','header_validation_failures','abandoned']:
        if c.get(k, 0): bad.append(f'{p}: {k}={c[k]}')
    if c.get('contaminated') or c.get('drain_bound_hit'): bad.append(p+': contamination/drain bound')
    if c['completed'] != c['target_stats']['received']: bad.append(p+': completed/received mismatch')
assert not bad, bad
lines += ['', 'All 24 throughput cells passed result validation: zero errors, header failures, rejected messages, abandoned messages and completed/final-received mismatches after drain; no contamination or drain-bound hit. '
          'Per-round inputs, source/target logs and results are under `ab/`; command order is in `ab.log` and `ab.sh`.', '',
          '## Build, tests and formatting', '',
          '- Release benchmark built initially with `framework/bench/grpc/cpp/build.sh` inside cpp-build-slot.sh. '
          'The final benchmark, copy probe and every default C++ target compiled with `--parallel 3 -- -k 0` under the required build wrapper and shared measurement lock. '
          'The final full target build includes the C++ cross-language host. Six affected test executable targets, including the largest files, were built in sequence, each using --parallel 3 and -k 0, followed by the all-target build, to preserve memory headroom. See `full-build-attempt-1.log` and the successful `final-recompile.log`. The first full build collected all initializer-list/span caller errors with -k 0; all were fixed and rebuilt.',
          '- Unit configuration uses creation probes ON, installation OFF, cross-language host ON, and the already installed reference vcpkg/Core/binding packages. '
          'Benchmark uses creation probes OFF. No Core/binding source was changed.',
          '- Six focused CTest targets passed through measure-shared.sh; see `focused-final.log` and `focused.xml`.',
          '- All 296 tests selected by unit|contract labels passed through measure-shared.sh, including all 287 framework-unit|framework-contract tests; '
          'the selected names match the pre-change list. See `unit-contract-final.log` and `unit-contract.xml`.',
          '- Added regressions cover shared receiver copy allocation/lifetime, separate public failure and late physical decoding, completion identity, terminal registration order during reentry, '
          'coallocated source lifetime, callback capture destruction before retained scheduler storage, raw inline callback copy/move/throw/empty semantics, borrowed target lifetime, '
          'and serializer replacement during encoding. Four allocation-reference primitives were updated from removed owning copies to borrowed bytes/media and the target held in the projection. All equality and behavior assertions remain unchanged; the references now enforce the lower allocation count.',
          '- The Debug send-allocation target also passed, including its GNU Debug-only shared-state stack classification check, which is conditionally skipped in Release. The Release send reference now borrows the routing id; both count and bytes equalities remain unchanged. See `send-fix.log`, `send-debug.log` and `send-fix-build.log`.',
          '- The standalone before/after receiver-copy probe measured 1 → 0 heap calls. Its deliberately allocating vector-copy control measured 1 → 1. '
          'See `copy-probe-final.log` and `probe/`.',
          '- `bash scripts/format/format.sh --check cpp` passed from the worktree root; `git diff --check` passed. See `format-final.log`.', '',
          '## Contract ownership and review', '',
          '- Owner: Framework task/receiver lifecycle, codecs and operation registry. Core/binding transport policy remains unchanged.',
          '- Spec: `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:38` (§2), `:329` (§9), `:375` (§10) and `:490` (§11); '
          '`02-handler-turn-and-execution-gate.ko.md` owns execution permission. Public and physical reply states remain distinct; first completion, physical conversion, identity, ordering and capture destruction boundaries are retained.',
          '- Cross-language comparison: .NET ZLinkRequestCompletion/ZLinkMeshCompletionTable, Java ZLinkJavaRawServicePort/ZLinkServiceOperationRegistry, and Node request-runtime/operation-registry '
          'retain the same single completion claim ownership and late physical conversion semantics. This is source comparison; only C++ tests were run. No behavior/spec gap was found that requires a multi-language runtime change.',
          '- Classification: A — representation adaptation to the existing contract. No spec change.', '',
          'Audited decision locations: **10 before → 9 after; 0 added**. Eight contract locations remain eight; the duplicate byte-order comparator owners decrease 2 → 1. The contract locations are task first-result selection, public admission-failure publication, physical-to-public projection, '
          'registry terminal claim, raw source admission eligibility, node-role classification, normal wait claim and owner-cancellation wait claim. '
          'No new execution-permission flag, timer, retry, queue, lock, pool policy or fallback path was added. '
          'The optional callback/serializer storage controls object lifetime, not execution permission. '
          'The fixed raw callback has compile-time size/alignment/copy/move checks and no heap fallback. '
          'The per-node outer wire vector replaces per-request outer-vector allocation, and terminal-list nodes replace the old callback vector plus boxes.', '',
          'Refactoring review covered every changed module and its request call path for performance, POSDDD ownership and unused code. '
          'Performance review found and fixed 13 representation issues: nested callable cloning; separately owned request, admission source/state and target/projection; unused default media; copied selected media; separate completion registration/callback; boxed terminal observers; header growth; topology and host RID vectors; per-call outer wire vector; and raw callback boxing. POSDDD/helper ownership findings: 1 duplicate byte comparator, consolidated into the existing comparator; unused plumbing/include findings: 2, both removed. No duplicate policy owner or execution gate bypass was introduced. '
          'Removed unused private request_type plumbing and the unused task.hpp vector include. No finding in the touched path was deferred. '
          'Every changed diff line was read before commit.', '',
          '## Execution constraint violation and unfinished work', '',
          'The memory-floor requirement was **not maintained throughout**. During overlapping early builds, MemAvailable was observed at 10.568 GiB and then **8.551 GiB**, below the required 12 GiB. '
          'Two own compiler processes were terminated and the own Ninja scheduler paused. Memory recovered; both interrupted translation units subsequently compiled successfully with -k 0. '
          'Later builds reserved both C++ build slots while retaining the shared measurement lock; a later large compile was briefly paused/resumed proactively without another observed violation. '
          'See `memory-incident.txt`, `final-build-memory.tsv`, `final-rebuild-memory.tsv` and `send-rebuild-memory.tsv`. This historical constraint violation cannot be undone.', '',
          'No remaining implementation, test, formatting, census or requested A/B work. No push was performed.', '',
          'Changed files: request/task and channel/call contract headers; raw route adapter; channel runtime; mesh owner/topology/runtime; client/envelope codec plumbing; '
          'node header encoder; public host request path; three C++ request unit-test files; four existing tests mechanically adapted to span arguments; and the send-allocation reference. The report and measurement artifacts remain outside the repository.', '']

for f in ['focused.xml', 'unit-contract.xml', 'send-fix.xml', 'send-debug.xml']:
    root = ET.parse(job / f).getroot()
    suites = [root] if root.tag == 'testsuite' else root.findall('testsuite')
    assert sum(int(s.get('failures', 0)) + int(s.get('errors', 0)) for s in suites) == 0, f
    assert sum(int(s.get('skipped', 0)) for s in suites) == 0, f
assert after['submitter']['calls_per_message'] <= 30
(job / 'report.md').write_text('\n'.join(lines))
print(job / 'report.md')
