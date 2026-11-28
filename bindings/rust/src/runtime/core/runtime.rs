use crate::contracts::messaging::message::Message;

pub fn multipart_close(parts: &mut [Message]) {
    for part in parts.iter_mut() {
        part.close_now();
    }
}
