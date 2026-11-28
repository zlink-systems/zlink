// SPDX-License-Identifier: MPL-2.0

use crate::internal::SocketStorage;
use crate::{
    BindError, CommonSocketOptions, ConfigError, ConnectError, Empty, Received, RecvError,
    RecvFlags, SendOp,
};

/// PAIR socket, a bidirectional one-to-one messaging socket.
pub struct PairSocket {
    pub(crate) inner: Box<SocketStorage>,
}

impl std::panic::UnwindSafe for PairSocket {}
impl std::panic::RefUnwindSafe for PairSocket {}

impl PairSocket {
    /// Begins a multipart send: add parts on the returned builder, then submit.
    /// A part is consumed on a successful submit (see [`SendOp`]).
    pub fn send(&self) -> SendOp<Empty> {
        self.inner.socket_send_op()
    }

    /// Receives a message into caller-provided `out` storage, reusable across
    /// calls.
    ///
    /// Returns `Ok(true)` on success and `Ok(false)` when
    /// [`RecvFlags::DONT_WAIT`] is set and no message is available.
    pub fn recv(&self, out: &mut Received, flags: RecvFlags) -> Result<bool, RecvError> {
        self.inner.recv(out, flags)
    }

    /// Returns the typed options facade common to all socket types.
    pub fn common_options(&self) -> CommonSocketOptions<'_> {
        CommonSocketOptions::new(&self.inner)
    }
}

impl_socket_common_base!(PairSocket);
impl_socket_connections!(PairSocket);
