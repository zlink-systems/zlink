use crate::contracts::messaging::message::Message;

pub(crate) struct MessageParts {
    inline: [crate::native::ffi::zlink_msg_t; 2],
    inline_len: usize,
    heap: Option<Vec<crate::native::ffi::zlink_msg_t>>,
}

impl Default for MessageParts {
    fn default() -> Self {
        Self {
            inline: [
                crate::native::ffi::zlink_msg_t::recv_slot(),
                crate::native::ffi::zlink_msg_t::recv_slot(),
            ],
            inline_len: 0,
            heap: None,
        }
    }
}

impl MessageParts {
    pub(crate) fn push(&mut self, message: Message) {
        if let Some(parts) = self.heap.as_mut() {
            parts.push(message.into_raw());
            return;
        }

        if self.inline_len < self.inline.len() {
            self.inline[self.inline_len] = message.into_raw();
            self.inline_len += 1;
            return;
        }

        let mut parts = Vec::with_capacity(self.inline_len + 1);
        for slot in self.inline.iter_mut().take(self.inline_len) {
            parts.push(std::mem::replace(
                slot,
                crate::native::ffi::zlink_msg_t::recv_slot(),
            ));
        }
        self.inline_len = 0;
        parts.push(message.into_raw());
        self.heap = Some(parts);
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub(crate) fn len(&self) -> usize {
        self.heap
            .as_ref()
            .map_or(self.inline_len, |parts| parts.len())
    }

    pub(crate) fn iter_mut(&mut self) -> std::slice::IterMut<'_, crate::native::ffi::zlink_msg_t> {
        self.as_mut_slice().iter_mut()
    }

    pub(crate) fn as_mut_ptr(&mut self) -> *mut crate::native::ffi::zlink_msg_t {
        self.as_mut_slice().as_mut_ptr()
    }

    pub(crate) fn clear(&mut self) {
        if let Some(parts) = self.heap.as_mut() {
            parts.clear();
        } else {
            self.inline_len = 0;
        }
    }

    fn as_mut_slice(&mut self) -> &mut [crate::native::ffi::zlink_msg_t] {
        match self.heap.as_mut() {
            Some(parts) => parts.as_mut_slice(),
            None => &mut self.inline[..self.inline_len],
        }
    }
}
