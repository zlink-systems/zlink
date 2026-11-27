// SPDX-License-Identifier: MPL-2.0

package contracts

// CompletionKind identifies a native completion record kind. Send is ABI-only;
// Writable tells a managed send to retry its retained packet.
//
// Deprecated: completion records are internal to the binding; nothing in the
// public API accepts or returns a CompletionKind.
type CompletionKind int

const (
	// Deprecated: internal completion record kind.
	//
	// CompletionSend is retained for ABI compatibility and is never emitted.
	CompletionSend CompletionKind = 1
	// Deprecated: internal completion record kind.
	//
	// CompletionRequest carries a completed request and its reply or terminal result.
	CompletionRequest CompletionKind = 2
	// Deprecated: internal completion record kind.
	//
	// CompletionWritable carries the exact wait token whose send target became writable.
	CompletionWritable CompletionKind = 3
)
