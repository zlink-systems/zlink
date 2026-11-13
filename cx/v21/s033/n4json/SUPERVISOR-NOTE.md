# Shared fact for node-l57g and n4json (supervisor, 11-13 21:10)

Checked at df42a4df40: Node default JSON (framework-json-v1) does NOT write Buffer as {type,data:[...]}.
- Buffer is a Uint8Array → framework/languages/node/packages/stream-wire/src/internal/framework-json-v1.ts:63-64 encodeBase64 (padded RFC 4648, spec 05-message-model.ko.md:153). toJSON objects are rejected (:69-74).
- encodeBase64 :394-400 builds a binary string with String.fromCharCode(...chunk) + btoa; decodeBase64 :402-406 atob + per-char loop into a new Uint8Array (decoded value is Uint8Array, not Buffer).
- channel-envelope.ts:498-509: a top-level Buffer/Uint8Array value goes out as binary content type, an object (BenchPayload {body}) as JSON via stringifyFrameworkJsonV1 with the packet's JSON contract schema.

node-l57g: so the L5~L7 "received 0" is not a {type,data} wire mismatch; look at handler match, schema/contract registration and decode failure on the target.
n4json: the per-byte cost candidates above are on your path; this is a fact, not a conclusion — profile decides.

# PM 11-13 21:35 — .NET is the reference implementation
.NET keeps the same contract (default JSON, padded base64 byte sequences, same bench DTO) with L4 loss 10-41% and L4÷binding 0.56-0.78 (lad-dotnet-s1). "Remaining cost is required by the contract" is not acceptable while .NET does the same contract cheaper.
In report.md add a table: for each cost on your language's typed JSON path (encode, base64, decode, copies, allocations), the .NET equivalent file:line under framework/languages/dotnet/src/Zlink.Framework/ (e.g. System.Text.Json Utf8JsonWriter/Reader writing base64 straight into a pooled buffer) and what .NET does less. Fold that difference into the fix proposal.

# Supervisor 11-13 22:10 — do not leave Buffer.toJSON's Array(N)
Your report (:79) keeps the temporary Buffer.toJSON Array(N) per byte field (4096 numbers per 4K message, then discarded). .NET writes base64 straight from the bytes. Find a way to avoid it without new rules (e.g. the stringify path detecting Uint8Array before JSON.stringify invokes toJSON — a pre-pass over the typed object's own byte fields, or a writer for the schema'd path) and measure its share separately (alloc sampling) before deciding; if you still keep it, give the measured share and the rule cost of removing it.
