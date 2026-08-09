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
| Java/Kotlin | The source-side seal is implemented: the relocation barrier seals the execution lane and records the accepted session sequence at that point, which travels with the relocation. Missing are the session-owner-side seal (commands 42/43) and the ingress hold for messages that arrive after the seal. The owner is never told where the seal fell, so it has no reference value to compare against, and the high-water gate is monotonic rather than an equality — a high-water that regressed below an already applied one is a superseded relocation and is rejected. Messages arriving on the previous route after the seal are carried to the target Actor queue by Message Follow. | Implement commands 42/43 and the ingress hold so the owner records the seal boundary, then switch to the equality comparison |

The Java/Kotlin monotonic gate applies only to requests that already passed the ObjectGeneration,
AuthorityOwnerGeneration, and binding generation checks that precede it. A late-arriving route
request from an earlier relocation is rejected by those fences and by the monotonic gate.
