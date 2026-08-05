/// The outcome of a non-blocking send.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SendResult {
    /// The message was queued for sending.
    Sent,
    /// The send was refused because the outbound queue was full.
    Backpressured,
    /// The socket was not ready to accept the send.
    NotReady,
}

impl SendResult {
    /// Returns `true` when the result is [`SendResult::Sent`].
    pub fn is_sent(&self) -> bool {
        matches!(self, Self::Sent)
    }
}
