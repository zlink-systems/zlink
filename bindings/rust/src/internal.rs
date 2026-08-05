// SPDX-License-Identifier: MPL-2.0

//! Crate-private storage and lifecycle policies shared by public contracts and
//! runtime implementations.

mod callback_lifecycle;
mod deferred_cleanup;
mod handle_storage;
mod message_storage;

pub(crate) use callback_lifecycle::CallbackBox;
pub(crate) use deferred_cleanup::{DeferredCloseKind, defer_native_close, release_callbacks};
pub(crate) use handle_storage::{
    ContextStorage, MonitorStorage, PollerStorage, SocketStorage, TimerStorage,
};
pub(crate) use message_storage::MessageStorage;
