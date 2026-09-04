// SPDX-License-Identifier: MPL-2.0

// Package zlink provides idiomatic Go bindings for the zlink messaging library.
//
// The public surface follows the Core 0.17.0 raw-socket contract:
//   - multipart-only public send/receive APIs
//   - send builders make one DONTWAIT attempt, retain the logical packet after
//     back-pressure, and retry it after the exact WRITABLE token is pulled
//   - admitted sends have completion ID zero and never emit SEND completion;
//     requests retain their completion-backed reply terminal
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
//   - public poller Wait drains WRITABLE and REQUEST records to no-data;
//     WRITABLE-only wakes remain visible as POLLOUT, not POLLCOMPLETION
//
// Message ownership follows the native contract at the binding boundary.
// Message(...) submits a binding-owned staging copy, preserving the caller's
// message on a hard initial failure and consuming it once Core admits the send
// or returns a valid WRITABLE wait token. MoveMessage(...) transfers ownership
// to the operation at submit time. Bytes(...) snapshots caller-owned bytes
// during Submit. Core consumes each attempted native part but retains no SEND
// payload; the binding owns its immutable retry packet until admission,
// cancellation, or terminal failure. Receive paths transfer ownership to Go
// wrappers that must be closed explicitly when their lifetime ends.
//
// Message.Data returns a zero-copy view over native message storage. The view
// is valid only while the Message remains open. Use Message.Bytes when payload
// data must outlive the Message or cross a goroutine boundary.
package zlink
