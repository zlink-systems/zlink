use crate::domain::Received;
use crate::messaging_operations::{Empty, ReplyOp, SendOp};

pub(crate) fn received_reply(received: &Received) -> ReplyOp<Empty> {
    let (admission, routing_id, request_seq) =
        received.reply_target().expect("missing reply context");
    crate::operations::router_reply_op(admission, routing_id, request_seq)
}

pub(crate) fn received_send(received: &Received) -> SendOp<Empty> {
    let (handle, admission, routing_id) = received.send_target().expect("missing send context");
    match admission {
        Some(admission) => crate::operations::immediate_routed_send_op(admission, routing_id),
        None => crate::operations::stream_send_to_op(handle, routing_id),
    }
}
