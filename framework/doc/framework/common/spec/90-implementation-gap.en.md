# Framework Implementation Differences

This document records differences between the formal Framework contract and current language
implementations. An implementation status listed here does not narrow the public contract. The common
specifications and language exact interfaces define public behavior.

## Session Actor Binding Replacement

Binding the same Actor to a new session makes the new binding current immediately and does not wait
for the previous session's acknowledgment, callback, or close. The framework sends command 51 one-way
to the previous exact session. The previous session enters closing and runs the application callback.
At callback terminal it schedules a non-blocking timer, releases the turn immediately, revalidates the
exact retired identity, and closes the connection at 100 ms. It never implements the delay with sleep,
a blocking wait, or occupation of a session serial lane or worker.

| Language | Current implementation difference | Closure condition |
|---|---|---|
| C++ | None. Canonical commands 36/38 with the expected binding generation, one-way command 51, the public callback, and the non-blocking 100 ms timer are implemented. | Closed |
| Node.js | None. The new binding becomes current without waiting for acknowledgment and is never rolled back; one-way command 51, the public callback, and the non-blocking 100 ms timer are implemented. | Closed |
| .NET | None — publishes the new binding as current first and implements the command 51 one-way notification, the public callback, the exact retired fence, and the non-blocking 100 ms timer. | Closed — command 51 canonical schema/golden, owner regression, package, and process evidence passed |
| Java/Kotlin | None. The Java callback and the Kotlin suspending bridge, one-way command 51, and the non-blocking 100 ms timer are implemented; bind does not wait for the previous cleanup. | Closed |

Every language must produce the same result for the canonical and malformed bytes and the pre-restart
session-owner lifecycle rejection in `runtime/protocol/golden/bound-session-replaced-v1.json`. An
idempotent bind from the same physical session neither self-notifies nor closes that connection.

## Session relocation route high-water verification

Spec 20 §5 step 7 requires the session owner to verify, alongside the ObjectGeneration, the previous
and target AuthorityOwnerGeneration, the binding generation, and the session owner lease, that the
high-water in command 44 **equals** the value recorded on the current binding. That equality holds
because the `sessionRelocationSeal(42)` / `sessionRelocationSealed(43)` handshake freezes the
source's captured high-water and the owner's recorded high-water to the same number. Delivery of
messages that arrive on the previous route after the owner change is guaranteed by Message Follow,
not by this verification.

| Language | Current implementation difference | Closing condition |
|---|---|---|
| C++ | None. Frames commands 42/43 on the service wire. | Closed |
| Node.js | None. Implements the seal semantics in its in-process binding registry and correlates a route publish with its seal. | Closed |
| .NET | None. Implements the seal semantics over an internal relay and separates an exact-fence commit from an idempotent retransmit. | Closed |
| Java/Kotlin | Commands 42/43 are framed on the service wire and the equality is closed for every relocation that completes the handshake. The session owner answers command 42 with the accepted bound-session high-water it recorded at the seal, the source journals that ACK value in place of its own captured sequence, the target replays it in command 44, and the owner compares it for exact equality against the recorded seal. Two deliberate differences from C++: the owner compares the round-tripped seal token stored per relocation id instead of a live counter, and it installs no ingress barrier, so post-seal messages keep flowing on the previous route and reach the target Actor queue by Message Follow (spec 20 §5 step 5). A relocation that reaches the owner without a seal — a route rebuilt from a durable journal after a restart, or a source whose seal did not complete inside its deadline — keeps the monotonic gate. | Closed for sealed relocations; the restart-recovered route path still falls back to the monotonic gate |

The Java/Kotlin fallback gate applies only to requests that already passed the ObjectGeneration,
AuthorityOwnerGeneration, and binding generation checks that precede it. A late-arriving route
request from an earlier relocation is rejected by those fences and by the monotonic gate.

Command 45 carries a `session-relocation-route-result` field (applied / alreadyApplied / stale /
sessionOrBindingClosed). Spec 20 §5 requires the target to stop retransmitting once it receives any
of the four results, so a refused command 44 is answered with the reason instead of being left
unanswered. C++ and Java/Kotlin both encode and decode the field and answer `stale` or
`sessionOrBindingClosed` on their refusal paths.
