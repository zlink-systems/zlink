// SPDX-License-Identifier: MPL-2.0

package native

type RequestReplyCompletion struct {
	Result RequestResult
	Parts  []*Message
	Err    error
}

type requestResult struct {
	result RequestResult
	parts  []*Message
}
