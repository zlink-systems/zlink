# W-1 codec-generator pilot adoption

W-2 and W-3 progress is tracked in issue #736; the wire spec states only the
rule (no private syntax, no handwritten codec exceptions) and not this plan.

W-2 replaces each runtime's actorJoin(28) byte-layer call with the generated
module only after its language-local equivalence fixture is green.  The public
runtime types remain the adapter boundary; no generated type leaks through a
public API.

W-3 follows the same sequence for `relocation-envelope-v1`: map the runtime
envelope to the generated object tree at the owner boundary, use the generated
encoder/decoder for durable logical-stream bytes, then retain the current
chunking, checksum, and storage layers unchanged.  Keep the hand codec as the
byte-equivalence oracle until all language fixtures pass.

The currently generated logical-stream API accepts one complete array.  Chunk-level
incremental decoding is deferred to #778 and is a prerequisite for W-3 adapter replacement.

Stage 4 replaces adapters only after the final renderer output is present and
its language-local conformance fixture is green.  Replace `actorJoin(28)` first,
then `relocation-envelope-v1`, and only then move the remaining command and
durable-format adapters.  The legacy pilot generator and outputs remain in the
manifest until those adapter boundaries no longer import them.
