// SPDX-License-Identifier: MPL-2.0

use crate::ffi;

/// Owns the Core HWM credits retained by one received aggregate.
///
/// The raw lease type stays crate-private. Core permits release from a thread
/// other than the receive thread, so moving a received aggregate is safe.
#[derive(Default)]
pub(crate) struct HwmBudgetLeaseOwner {
    leases: Vec<*mut ffi::zlink_hwm_budget_lease_t>,
}

unsafe impl Send for HwmBudgetLeaseOwner {}

impl HwmBudgetLeaseOwner {
    pub(crate) fn adopt(&mut self, lease: *mut ffi::zlink_hwm_budget_lease_t) {
        if !lease.is_null() {
            self.leases.push(lease);
        }
    }

    pub(crate) fn release(&mut self) {
        for lease in self.leases.drain(..) {
            let mut lease = lease;
            unsafe {
                ffi::zlink_hwm_budget_lease_release(&mut lease);
            }
        }
    }
}

impl Drop for HwmBudgetLeaseOwner {
    fn drop(&mut self) {
        self.release();
    }
}
