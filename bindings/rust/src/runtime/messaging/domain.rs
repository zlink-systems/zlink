use crate::domain::Received;
use crate::messaging_operations::{Empty, ReplyOp, SendOp};

pub(crate) fn received_reply(received: &Received) -> ReplyOp<Empty> {
    let (routed, routing_id, request_seq) =
        received.reply_target().expect("missing reply context");
    crate::operations::router_reply_op(routed, routing_id, request_seq)
}

pub(crate) fn received_send(received: &Received) -> SendOp<Empty> {
    let (handle, routed, completions, routing_id) =
        received.send_target().expect("missing send context");
    match (routed, completions) {
        (Some(routed), Some(completions)) => {
            crate::operations::received_routed_send_op(routed, completions, routing_id)
        }
        (_, completions) => crate::operations::stream_send_to_op(handle, completions, routing_id),
    }
}
