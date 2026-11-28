impl crate::Received {
    pub(crate) fn receive_scratch(
        &mut self,
    ) -> (
        &mut Vec<crate::Message>,
        &mut Vec<crate::runtime::native::ffi::zlink_msg_t>,
    ) {
        (
            &mut self.receive_scratch,
            &mut self.native_receive_scratch.parts,
        )
    }
}

impl crate::TopicMessage {
    pub(crate) fn receive_scratch(
        &mut self,
    ) -> (
        &mut Vec<crate::Message>,
        &mut Vec<crate::runtime::native::ffi::zlink_msg_t>,
    ) {
        (
            &mut self.receive_scratch,
            &mut self.native_receive_scratch.parts,
        )
    }
}
