/// Legacy one-shot send classification retained for source compatibility.
///
/// Current SEND builders return `Result<(), SubmitError>`: managed
/// [`SendOp::submit`](crate::SendOp::submit) retries after WRITABLE, while
/// terminal failures use [`SubmitResult`](crate::SubmitResult).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SendResult {
    /// The message was queued for sending.
    Sent,
    /// The send was refused by HWM, missing credit, flow pause, or an unready
    /// existing target.
    Backpressured,
    /// Legacy compatibility value not emitted by the Core 0.17 SEND path.
    /// Missing ROUTER/STREAM routes are `SubmitResult::NotConnected`.
    NotReady,
}

impl SendResult {
    /// Returns `true` when the result is [`SendResult::Sent`].
    pub fn is_sent(&self) -> bool {
        matches!(self, Self::Sent)
    }
}

/// Identifies records in a socket completion queue.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum CompletionKind {
    /// ABI-reserved SEND value. Successful SEND admission has ID zero and
    /// does not enqueue a completion record.
    Send = 1,
    /// A REQUEST completed with a reply or terminal result.
    Request = 2,
    /// A DONTWAIT SEND or REQUEST wait token became eligible for one retry.
    ///
    /// This is permission to resubmit the same packet, not admission success.
    Writable = 3,
}
