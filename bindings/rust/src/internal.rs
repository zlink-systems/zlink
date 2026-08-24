// SPDX-License-Identifier: MPL-2.0

//! Crate-private storage and lifecycle policies shared by public contracts and
//! runtime implementations.

mod callback_lifecycle;
mod deferred_cleanup;
mod handle_storage;
mod receive_owner;
mod message_storage;
mod routed_handle;
mod send_completion;

pub(crate) use callback_lifecycle::CallbackBox;
pub(crate) use deferred_cleanup::{DeferredCloseKind, defer_native_close, release_callbacks};
pub(crate) use handle_storage::{
    ContextStorage, MonitorStorage, PollerStorage, SocketStorage, TimerStorage,
};
pub(crate) use receive_owner::ReceiveOwner;
pub(crate) use message_storage::MessageStorage;
pub(crate) use routed_handle::{RoutedHandle, RoutedRole};
pub(crate) use send_completion::{
    SendCompletionSlot, SendCompletions, send_complete_trampoline,
};
