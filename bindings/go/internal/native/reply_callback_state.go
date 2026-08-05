// SPDX-License-Identifier: MPL-2.0

package native

import (
	"sync"
)

// replyCallbackState carries an async request's reply state across the cgo
// callback boundary. The result is stored once, then completion hooks run
// after the native callback has delivered the result.
type replyCallbackState struct {
	result             requestResult
	once               sync.Once
	mu                 sync.Mutex
	progressDetach     func()
	progressDone       bool
	completionDispatch func(requestResult)
	completionDone     bool
}

func (s *replyCallbackState) complete(result requestResult) {
	s.once.Do(func() {
		s.mu.Lock()
		s.result = result
		s.progressDone = true
		detach := s.progressDetach
		s.progressDetach = nil
		s.completionDone = true
		dispatch := s.completionDispatch
		s.completionDispatch = nil
		s.mu.Unlock()
		if detach != nil {
			detach()
		}
		if dispatch != nil {
			dispatch(result)
		}
	})
}

func (s *replyCallbackState) setProgressDetach(detach func()) {
	if s == nil || detach == nil {
		return
	}
	s.mu.Lock()
	if s.progressDone {
		s.mu.Unlock()
		detach()
		return
	}
	s.progressDetach = detach
	s.mu.Unlock()
}

func (s *replyCallbackState) setCompletionDispatch(dispatch func(requestResult)) {
	if s == nil || dispatch == nil {
		return
	}
	s.mu.Lock()
	if s.completionDone {
		result := s.result
		s.mu.Unlock()
		dispatch(result)
		return
	}
	s.completionDispatch = dispatch
	s.mu.Unlock()
}

func newReplyCallbackState() *replyCallbackState {
	return &replyCallbackState{}
}
