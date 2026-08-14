// SPDX-License-Identifier: MPL-2.0

package native

/*
#include "zlink.h"
*/
import "C"

import "sync"

// hwmBudgetLeaseOwner is the single private owner for every physical-part
// credit attached to one retained aggregate receive result. It deliberately
// exposes neither the native handles nor a second accounting model.
type hwmBudgetLeaseOwner struct {
	mu     sync.Mutex
	leases []*C.zlink_hwm_budget_lease_t
	closed bool
}

func newHwmBudgetLeaseOwner() *hwmBudgetLeaseOwner {
	return &hwmBudgetLeaseOwner{leases: make([]*C.zlink_hwm_budget_lease_t, 0, 1)}
}

// reserveOne makes the next adoption allocation-free. The receive loop calls
// it before Core can dequeue the corresponding physical part.
func (o *hwmBudgetLeaseOwner) reserveOne() {
	if o == nil {
		return
	}
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.closed || len(o.leases) < cap(o.leases) {
		return
	}
	nextCapacity := cap(o.leases) * 2
	if nextCapacity == 0 {
		nextCapacity = 1
	}
	leases := make([]*C.zlink_hwm_budget_lease_t, len(o.leases), nextCapacity)
	copy(leases, o.leases)
	o.leases = leases
}

func (o *hwmBudgetLeaseOwner) adopt(lease *C.zlink_hwm_budget_lease_t) {
	if lease == nil {
		return
	}
	if o == nil {
		C.zlink_hwm_budget_lease_release(&lease)
		return
	}
	o.mu.Lock()
	if o.closed {
		o.mu.Unlock()
		C.zlink_hwm_budget_lease_release(&lease)
		return
	}
	o.leases = append(o.leases, lease)
	o.mu.Unlock()
}

func (o *hwmBudgetLeaseOwner) release() {
	if o == nil {
		return
	}
	o.mu.Lock()
	if o.closed {
		o.mu.Unlock()
		return
	}
	o.closed = true
	leases := o.leases
	o.leases = nil
	for _, lease := range leases {
		C.zlink_hwm_budget_lease_release(&lease)
	}
	o.mu.Unlock()
}
