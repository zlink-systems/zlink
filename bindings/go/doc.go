// SPDX-License-Identifier: MPL-2.0

// Package zlink provides idiomatic Go bindings for the zlink messaging library.
//
// The public surface follows the Core 0.16.0 raw-socket contract:
//   - multipart-only public send/receive APIs
//   - send and request builders expose one context-aware completion terminal
//   - publish alone retains send flags and its `(bool, error)` result
//   - non-blocking receive reports only "no data" via the ok result
//   - ROUTER requests carry opaque owner-bound reply tokens
//   - STREAM packet, monitor, and timer delivery are pull-based
//   - context options are exposed through Context.Options() as ContextOptions
//   - socket capabilities are split by concrete socket type
//   - typed domain objects model routing IDs, aggregate receive results, topic
//     messages, subscription events, and monitor events
//   - raw option bags and raw flags are not exposed publicly
//   - monitor open APIs use typed event masks and default to ALL when omitted
//   - poller completion readiness advances the socket-local completion owner
//
// Message ownership follows the native contract at the binding boundary.
// Message(...) submits a binding-owned staging copy, preserving the caller's
// message on submit failure and consuming it on success; Core still consumes
// the staged native part on ordinary synchronous failure. MoveMessage(...)
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
