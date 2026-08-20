# W-1 codec-generator pilot adoption

W-2 replaces each runtime's actorJoin(28) byte-layer call with the generated
module only after its language-local equivalence fixture is green.  The public
runtime types remain the adapter boundary; no generated type leaks through a
public API.

W-3 follows the same sequence for `relocation-envelope-v1`: map the runtime
envelope to the generated object tree at the owner boundary, use the generated
encoder/decoder for durable logical-stream bytes, then retain the current
chunking, checksum, and storage layers unchanged.  Keep the hand codec as the
byte-equivalence oracle until all language fixtures pass.
