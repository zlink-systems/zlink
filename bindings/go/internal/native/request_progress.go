// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include "zlink.h"
*/
import "C"

import (
	"runtime"
	"sync"
	"unsafe"
)

// Per-handle progress pump: a single goroutine drives request progress for
// all in-flight requests on the same native handle, using a bounded poll
// interval (matches README.godoc.md: do not
// start one polling thread per request when progress can be shared per handle).
const (
	pollCompletionEvent   = C.short(32)
	progressPollTimeoutMs = C.long(50)
)

type progressPump struct {
	handle      unsafe.Pointer
	mu          sync.Mutex
	activeCount int
	workerOn    bool
	stopping    bool
	stopped     *sync.Cond
}

var (
	externalProgressMu   sync.Mutex
	externalProgressRefs = make(map[unsafe.Pointer]int)
)

func newProgressPump(handle unsafe.Pointer) *progressPump {
	pump := &progressPump{handle: handle}
	pump.stopped = sync.NewCond(&pump.mu)
	return pump
}

func acquireExternalRequestProgress(handle unsafe.Pointer, owner *socketCore) {
	if handle == nil {
		return
	}
	externalProgressMu.Lock()
	externalProgressRefs[handle]++
	externalProgressMu.Unlock()
	if owner != nil {
		owner.pauseInternalRequestProgress()
	}
}

func releaseExternalRequestProgress(handle unsafe.Pointer, owner *socketCore) {
	if handle == nil {
		return
	}
	externalProgressMu.Lock()
	last := false
	if count := externalProgressRefs[handle]; count <= 1 {
		delete(externalProgressRefs, handle)
		last = true
	} else {
		externalProgressRefs[handle] = count - 1
	}
	externalProgressMu.Unlock()
	if last && owner != nil {
		owner.resumeInternalRequestProgress()
	}
}

func externalRequestProgressActive(handle unsafe.Pointer) bool {
	externalProgressMu.Lock()
	active := externalProgressRefs[handle] > 0
	externalProgressMu.Unlock()
	return active
}

func (p *progressPump) attach(state *replyCallbackState) {
	p.mu.Lock()
	p.activeCount++
	startWorker := !p.workerOn && !p.stopping
	if startWorker {
		p.workerOn = true
	}
	p.mu.Unlock()
	state.setProgressDetach(p.detach)
	if startWorker {
		go p.run()
	}
}

func (p *progressPump) detach() {
	p.mu.Lock()
	if p.activeCount > 0 {
		p.activeCount--
	}
	p.mu.Unlock()
}

func (p *progressPump) active() bool {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.activeCount > 0 && !p.stopping
}

func (p *progressPump) workerStopped() {
	p.mu.Lock()
	if p.activeCount > 0 && !p.stopping {
		// A request arrived while the worker was leaving. Keep ownership of
		// the worker slot and restart it after the native poller is released.
		p.mu.Unlock()
		go p.run()
		return
	}
	p.workerOn = false
	p.stopped.Broadcast()
	p.mu.Unlock()
}

func (p *progressPump) stopAndWait() {
	p.mu.Lock()
	p.stopping = true
	for p.workerOn {
		p.stopped.Wait()
	}
	p.mu.Unlock()
}

func (p *progressPump) resume() {
	p.mu.Lock()
	p.stopping = false
	startWorker := p.activeCount > 0 && !p.workerOn
	if startWorker {
		p.workerOn = true
	}
	p.mu.Unlock()
	if startWorker {
		go p.run()
	}
}

func (p *progressPump) run() {
	defer p.workerStopped()
	poller := C.zlink_poller_new()
	if poller == nil {
		return
	}
	defer C.zlink_poller_destroy(&poller)
	if C.zlink_poller_add(poller, p.handle, nil, pollCompletionEvent) != C.ZLINK_CONFIG_OK {
		return
	}
	defer C.zlink_poller_remove(poller, p.handle)
	var event C.zlink_poller_event_t
	var pollError C.zlink_config_result_t
	for p.active() {
		if C.zlink_poller_wait(poller, &event, 1, progressPollTimeoutMs, &pollError) < 0 {
			break
		}
		runtime.Gosched()
	}
}
