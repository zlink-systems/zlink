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
| C++ | Uses a separate JSON packet instead of canonical commands 36/38 and discards the expected binding generation. Command 51 and the public callback are absent. | Commands 36/38 conformance, command 51, callback, non-blocking 100 ms timer, and process rebind evidence |
| Node.js | Waits for previous tombstone-cleanup acknowledgment and rolls back the new binding on failure. Command 51 and the public callback are absent. | No-ACK transition, command 51, callback, non-blocking 100 ms timer, and package/process evidence |
| .NET | Switches the route after confirming previous binding cleanup. Command 51 and the public callback are absent. | Command 51, callback, exact retired fence, non-blocking 100 ms timer, and package/process evidence |
| Java/Kotlin | Completes bind after the previous cleanup callback terminal. Command 51 and both language callbacks are absent. | Java/Kotlin callback bridge, command 51, non-blocking 100 ms timer, and package/process evidence |

Every language must produce the same result for the canonical and malformed bytes and the pre-restart
session-owner lifecycle rejection in `runtime/protocol/golden/bound-session-replaced-v1.json`. An
idempotent bind from the same physical session neither self-notifies nor closes that connection.
