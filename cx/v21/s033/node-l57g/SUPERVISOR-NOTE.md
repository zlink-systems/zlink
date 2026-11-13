# Shared fact for node-l57g and n4json (supervisor, 11-13 21:10)

Checked at df42a4df40: Node default JSON (framework-json-v1) does NOT write Buffer as {type,data:[...]}.
- Buffer is a Uint8Array → framework/languages/node/packages/stream-wire/src/internal/framework-json-v1.ts:63-64 encodeBase64 (padded RFC 4648, spec 05-message-model.ko.md:153). toJSON objects are rejected (:69-74).
- encodeBase64 :394-400 builds a binary string with String.fromCharCode(...chunk) + btoa; decodeBase64 :402-406 atob + per-char loop into a new Uint8Array (decoded value is Uint8Array, not Buffer).
- channel-envelope.ts:498-509: a top-level Buffer/Uint8Array value goes out as binary content type, an object (BenchPayload {body}) as JSON via stringifyFrameworkJsonV1 with the packet's JSON contract schema.

node-l57g: so the L5~L7 "received 0" is not a {type,data} wire mismatch; look at handler match, schema/contract registration and decode failure on the target.
n4json: the per-byte cost candidates above are on your path; this is a fact, not a conclusion — profile decides.
