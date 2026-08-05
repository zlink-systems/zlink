// SPDX-License-Identifier: MPL-2.0

// Package zlink provides idiomatic Go bindings for the zlink messaging library.
//
// The public surface follows the Core 11 raw-socket contract:
//   - multipart-only public send/receive APIs
//   - blocking methods use direct names and non-blocking methods use Try*
//   - non-blocking submit returns `(false, nil)` only for temporary backpressure
//   - non-blocking receive reports only "no data" via the ok result
//   - message diagnostics expose only the properties provided by the Core 11 raw API
//   - context options are exposed through Context.Options() as ContextOptions
//   - socket capabilities are split by concrete socket type
//   - typed domain objects model routing IDs, aggregate receive results, topic
//     messages, subscription events, completion-control records, and monitor events
//   - raw option bags and raw flags are not exposed publicly
//   - monitor open APIs use typed event masks and default to ALL when omitted
//   - callback delivery hops from native threads onto Go-managed dispatcher
//     goroutines before invoking user handlers
//
// Message ownership follows the native contract. Message(...) preserves the
// caller's message on submit failure and consumes it on success. MoveMessage(...)
// transfers ownership to the operation at submit time and is intended for hot
// paths that do not need to reuse a failed-send payload. Bytes(...) reads
// caller-owned bytes during Submit and does not retain the slice after Submit
// returns. Receive paths transfer ownership to Go wrappers that must be closed
// explicitly when their lifetime ends.
//
// Message.Data returns a zero-copy view over native message storage. The view
// is valid only while the Message remains open. Use Message.Bytes when payload
// data must outlive the Message or cross a goroutine boundary.
package zlink
