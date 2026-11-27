// SPDX-License-Identifier: MPL-2.0

//! Completion registry state. The native calls that drive it live in
//! `runtime::messaging::completion_owner`.

use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicUsize};
use std::sync::{Arc, Condvar, Mutex};
use std::task::Waker;

use crate::contracts::errors::errors::{RequestError, SubmitError};
use crate::contracts::messaging::message::Message;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum CompletionEntryKind {
    SendRetry,
    Request,
}

pub(crate) enum CompletionOutcome {
    Writable {
        completion_id: u64,
        result: Result<(), SubmitError>,
    },
    Request(Result<Vec<Message>, RequestError>),
}

pub(crate) struct EntryState {
    pub(crate) completion_id: u64,
    pub(crate) published: bool,
    pub(crate) captured: bool,
    pub(crate) settled: bool,
    pub(crate) detached: bool,
    pub(crate) owner_shutdown: bool,
    pub(crate) awaiting_writable: bool,
    pub(crate) admission_failure: Option<SubmitError>,
    pub(crate) reply_detached: bool,
    pub(crate) outcome: Option<CompletionOutcome>,
    pub(crate) writable_waker: Option<Waker>,
    pub(crate) request_waker: Option<Waker>,
}

/// One provisional send or request operation.
pub(crate) struct CompletionEntry {
    pub(crate) kind: CompletionEntryKind,
    pub(crate) state: Mutex<EntryState>,
    pub(crate) changed: Condvar,
}

pub(crate) struct OwnerState {
    pub(crate) entries: HashMap<usize, Arc<CompletionEntry>>,
    pub(crate) public_owner: Option<usize>,
}

/// The single drain owner and provisional registry for one native socket.
pub(crate) struct CompletionOwner {
    pub(crate) socket: usize,
    pub(crate) state: Mutex<OwnerState>,
    /// Serializes completion pulls (public drain and inline blocking request)
    /// with owner changes. Submits never take it: an entry is registered before
    /// the native call and a completion that arrives first joins the submit
    /// result inside the entry.
    pub(crate) completion_gate: Mutex<()>,
    /// Set once Core accepted the close. Registration checks it under `state`.
    pub(crate) closed: AtomicBool,
    pub(crate) next_context: AtomicUsize,
}

unsafe impl Send for CompletionOwner {}
unsafe impl Sync for CompletionOwner {}
