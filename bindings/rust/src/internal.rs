// SPDX-License-Identifier: MPL-2.0

//! Crate-private storage and lifecycle policies shared by public contracts and
//! runtime implementations.

mod completion_owner;
mod deferred_cleanup;
mod handle_storage;
mod message_storage;
mod routed_handle;

pub(crate) use completion_owner::{CompletionEntry, CompletionOwner};
pub(crate) use deferred_cleanup::{DeferredCloseKind, defer_native_close};
pub(crate) use handle_storage::{
    ContextStorage, MonitorStorage, PollerSocketRegistration, PollerStorage, SocketStorage,
    TimerStorage,
};
pub(crate) use message_storage::MessageStorage;
pub(crate) use routed_handle::RoutedHandle;

/// Heap identity shared by a ROUTER wrapper and every token it creates.
pub(crate) struct RouterOwnerTag;
