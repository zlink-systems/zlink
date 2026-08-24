// SPDX-License-Identifier: MPL-2.0

/// Compatibility carrier for receive aggregation state.
///
/// Core now returns ordinary message ownership at dequeue, so no native
/// receive resource remains to release.
#[derive(Default)]
pub(crate) struct ReceiveOwner;

unsafe impl Send for ReceiveOwner {}

impl ReceiveOwner {
    pub(crate) fn release(&mut self) {}
}
