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

## Session Relocation Route And Target-Only Cutover

Spec 30 and Spec 20 §5 require every language to use the same simple boundary. The Session
owner seals the exact binding with commands 42/43 and holds later Session messages. Message
order between source and target comes from relay and one-way cutover on the same TCP
connection, not a numeric high-water. Only target runs the Location Store CAS. After queue
opening it sends command 44 to the Session owner as `[send]`. Command 44 has no reply and the
normal flow doesn't use command 45. Without an exact update before
`SessionRelocationSealTimeout`, the Session owner cleans the physical Session and related
state.

| Language | Current implementation difference | Closing condition |
|---|---|---|
| C++ | Remove the previous command 45 ACK and high-water route application, then converge on target one-way command 44 and Session-timeout cleanup. | Open |
| Node.js | Remove aggregate high-water/route-ACK state, then converge on ordered relay, target one-way command 44, and Session-timeout cleanup. | Open |
| .NET | Remove exact-high-water and ACK-retry paths, then apply one-way command 44 after target-only CAS. | Open |
| Java/Kotlin | Keep commands 42/43 only as binding seal, remove high-water and command 45 terminal, and add target one-way command 44 plus Session-timeout cleanup. | Open |

The closing condition is identical for all four languages. Commands 42/43 only install
the current-binding seal, and command 44 is sent one-way after target CAS and queue opening.
The Session owner validates only exact Session, binding, and relocation identity; it
doesn't re-read Store or Actor authority. Timeout and route update are serialized so only
the first takes effect, while a late or duplicate update records a Warning. The gap is
closed only when command 45 and relocation-specific high-water disappear from production,
codec expectations, and the normal path of contract tests.
