use crate::domain::Received;
use crate::messaging_operations::{Empty, ReplyOp, SendOp};

pub(crate) fn received_reply(received: &Received) -> ReplyOp<Empty> {
    let (routed, owner, routing_id, token) =
        received.reply_target().expect("missing reply context");
    crate::operations::router_reply_op(routed, owner, routing_id, token)
}

pub(crate) fn received_send(received: &Received) -> SendOp<Empty> {
    let (handle, routed, completion_owner, routing_id) =
        received.send_target().expect("missing send context");
    match routed {
        Some(routed) => crate::operations::routed_send_op(routed, completion_owner, routing_id),
        None => crate::operations::stream_send_to_op(handle, completion_owner, routing_id),
    }
}
