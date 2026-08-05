use crate::domain::Received;
use crate::messaging_operations::{Empty, ReplyOp, SendOp};

pub(crate) fn received_reply(received: &Received) -> ReplyOp<Empty> {
    let (handle, routing_id, request_seq) = received.reply_target().expect("missing reply context");
    crate::operations::router_reply_op(handle, routing_id, request_seq)
}

pub(crate) fn received_send(received: &Received) -> SendOp<Empty> {
    let (handle, routing_id) = received.send_target().expect("missing send context");
    crate::operations::socket_send_to_op(handle, routing_id)
}
