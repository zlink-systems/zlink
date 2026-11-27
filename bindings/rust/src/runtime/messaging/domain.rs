use crate::messaging_operations::{Empty, ReplyOp, SendOp};

impl crate::Received {
    pub(crate) fn runtime_reply(&self) -> ReplyOp<Empty> {
        let (routed, owner, routing_id, token) =
            self.reply_target().expect("missing reply context");
        crate::messaging_operations::ReplyOpStorage::router_reply_op(
            routed, owner, routing_id, token,
        )
    }

    pub(crate) fn runtime_send(&self) -> SendOp<Empty> {
        let (handle, routed, completion_owner, routing_id) =
            self.send_target().expect("missing send context");
        match routed {
            Some(routed) => crate::messaging_operations::SendOpStorage::routed_send_op(
                routed,
                completion_owner,
                routing_id,
            ),
            None => crate::messaging_operations::SendOpStorage::stream_send_to_op(
                handle,
                completion_owner,
                routing_id,
            ),
        }
    }

    pub(crate) fn receive_scratch(
        &mut self,
    ) -> (&mut Vec<crate::Message>, &mut Vec<crate::ffi::zlink_msg_t>) {
        (
            &mut self.receive_scratch,
            &mut self.native_receive_scratch.parts,
        )
    }
}

impl crate::TopicMessage {
    pub(crate) fn receive_scratch(
        &mut self,
    ) -> (&mut Vec<crate::Message>, &mut Vec<crate::ffi::zlink_msg_t>) {
        (
            &mut self.receive_scratch,
            &mut self.native_receive_scratch.parts,
        )
    }
}
