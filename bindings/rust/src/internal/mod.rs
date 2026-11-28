// SPDX-License-Identifier: MPL-2.0

//! Crate-private storage and lifecycle policies shared by public contracts and
//! runtime implementations.

mod completion_state;
mod handle_storage;
mod message_parts;
mod message_storage;
mod routed_handle;

pub(crate) use completion_state::{
    CompletionEntry, CompletionEntryKind, CompletionOutcome, CompletionOwner, EntryState,
    OwnerState,
};
pub(crate) use handle_storage::{
    AtomicCounterStorage, ContextStorage, CoreUtilities, MonitorStorage, PollerSocketRegistration,
    PollerStorage, SocketStorage, StopwatchStorage, ThreadStorage, TimerStorage,
};
pub(crate) use message_parts::MessageParts;
pub(crate) use message_storage::{MessageStorage, NativeReceiveScratch};
pub(crate) use routed_handle::RoutedHandle;

/// Heap identity shared by a ROUTER wrapper and every token it creates.
pub(crate) struct RouterOwnerTag;
