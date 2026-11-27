// SPDX-License-Identifier: MPL-2.0

//! Crate-private storage and lifecycle policies shared by public contracts and
//! runtime implementations.

mod handle_storage;
mod message_parts;
mod message_storage;
mod routed_handle;

pub(crate) use crate::runtime::messaging::completion_owner::{CompletionEntry, CompletionOwner};
pub(crate) use handle_storage::{
    ContextStorage, MonitorStorage, PollerSocketRegistration, PollerStorage, SocketStorage,
    TimerStorage,
};
pub(crate) use message_parts::MessageParts;
pub(crate) use message_storage::{MessageStorage, NativeReceiveScratch};
pub(crate) use routed_handle::RoutedHandle;

/// Heap identity shared by a ROUTER wrapper and every token it creates.
pub(crate) struct RouterOwnerTag;
