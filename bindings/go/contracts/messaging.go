// SPDX-License-Identifier: MPL-2.0

package contracts

// CompletionKind identifies a native completion record kind. Send is ABI-only;
// Writable tells a managed send to retry its retained packet.
type CompletionKind int

const (
	// CompletionSend is retained for ABI compatibility and is never emitted.
	CompletionSend CompletionKind = 1
	// CompletionRequest carries a completed request and its reply or terminal result.
	CompletionRequest CompletionKind = 2
	// CompletionWritable carries the exact wait token whose send target became writable.
	CompletionWritable CompletionKind = 3
)
