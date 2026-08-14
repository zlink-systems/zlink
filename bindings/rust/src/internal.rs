// SPDX-License-Identifier: MPL-2.0

//! Crate-private storage and lifecycle policies shared by public contracts and
//! runtime implementations.

mod callback_lifecycle;
mod deferred_cleanup;
mod handle_storage;
mod hwm_budget_lease_owner;
mod message_storage;
mod routed_admission;

pub(crate) use callback_lifecycle::CallbackBox;
pub(crate) use deferred_cleanup::{DeferredCloseKind, defer_native_close, release_callbacks};
pub(crate) use handle_storage::{
    ContextStorage, MonitorStorage, PollerStorage, SocketStorage, TimerStorage,
};
pub(crate) use hwm_budget_lease_owner::HwmBudgetLeaseOwner;
pub(crate) use message_storage::MessageStorage;
pub(crate) use routed_admission::{
    DeadlineToken, RoutedAdmission, RoutedRole, RoutedTargetKey, RoutedWake, RoutedWakeOutcome,
    duration_until, routed_ready_trampoline,
};
