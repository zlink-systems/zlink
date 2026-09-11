use super::*;

const INITIAL_NATIVE_PART_CAPACITY: usize = 8;

// Short subscribe topics bypass heap allocation entirely (<=22 bytes live
// inline).
pub(crate) fn cstr_buf_to_smolstr(buf: &[i8], len: usize) -> smol_str::SmolStr {
    let bytes: &[u8] =
        unsafe { std::slice::from_raw_parts(buf.as_ptr() as *const u8, len.min(buf.len())) };
    match std::str::from_utf8(bytes) {
        Ok(s) => smol_str::SmolStr::new(s),
        Err(_) => smol_str::SmolStr::new(String::from_utf8_lossy(bytes)),
    }
}

pub(crate) fn routing_id_from_ptr(raw: *const ffi::zlink_routing_id_t) -> Option<RoutingId> {
    if raw.is_null() {
        None
    } else {
        RoutingId::from_raw_optional(unsafe { *raw })
    }
}

type RecvBasicParts = Result<Option<Option<RoutingId>>, RecvError>;
type RecvSubscribedParts = Result<Option<(Option<RoutingId>, smol_str::SmolStr)>, RecvError>;

/// Receive one whole record into reusable native storage, growing and retrying
/// without consuming the record when Core reports insufficient capacity.
pub(crate) fn recv_whole_message(
    parts: &mut Vec<Message>,
    native_parts: &mut Vec<ffi::zlink_msg_t>,
    flags: ffi::zlink_recv_flags_t,
    mut receive: impl FnMut(*mut ffi::zlink_msg_t, usize, *mut usize, ffi::zlink_recv_flags_t) -> i32,
) -> Result<bool, RecvError> {
    if native_parts.is_empty() {
        grow_native_parts(native_parts, INITIAL_NATIVE_PART_CAPACITY);
    }

    loop {
        let mut count = 0;
        let rc = receive(
            native_parts.as_mut_ptr(),
            native_parts.len(),
            &mut count,
            flags,
        );

        if rc == ffi::ZLINK_RECV_BUFFER_TOO_SMALL {
            if count <= native_parts.len() {
                return Err(RecvError::new(RecvResult::InternalError, libc::EPROTO));
            }
            grow_native_parts(native_parts, count);
            continue;
        }

        if rc == RecvResult::NoData as i32
            || (rc != 0 && unsafe { ffi::zlink_errno() } == libc::EAGAIN)
        {
            return Ok(false);
        }
        if rc != 0 {
            return Err(check_recv_rc(rc).unwrap_err());
        }
        if count == 0 || count > native_parts.len() {
            return Err(RecvError::new(RecvResult::InternalError, libc::EPROTO));
        }

        return adopt_native_parts(parts, native_parts, count).map(|()| true);
    }
}

fn grow_native_parts(parts: &mut Vec<ffi::zlink_msg_t>, capacity: usize) {
    parts.resize_with(capacity, ffi::zlink_msg_t::recv_slot);
}

fn adopt_native_parts(
    parts: &mut Vec<Message>,
    native_parts: &mut Vec<ffi::zlink_msg_t>,
    count: usize,
) -> Result<(), RecvError> {
    parts.clear();
    parts.reserve(count);

    for index in 0..count {
        let mut part = Message::new().map_err(|error| {
            unsafe {
                ffi::zlink_multipart_close(native_parts.as_mut_ptr(), count);
            }
            parts.clear();
            RecvError::new(RecvResult::InternalError, error.native_errno())
        })?;
        let rc = unsafe { ffi::zlink_msg_adopt(part.raw_mut(), &mut native_parts[index]) };
        if rc != 0 {
            let errno = unsafe { ffi::zlink_errno() };
            unsafe {
                ffi::zlink_multipart_close(native_parts.as_mut_ptr(), count);
            }
            parts.clear();
            return Err(RecvError::new(
                RecvResult::InternalError,
                if errno == 0 { libc::EIO } else { errno },
            ));
        }
        parts.push(part);
    }

    unsafe {
        ffi::zlink_multipart_close(native_parts.as_mut_ptr(), count);
    }
    Ok(())
}

pub(crate) fn recv_basic_parts(
    handle: *mut c_void,
    flags: ffi::zlink_recv_flags_t,
    parts: &mut Vec<Message>,
    native_parts: &mut Vec<ffi::zlink_msg_t>,
) -> RecvBasicParts {
    let mut routing_id = None;
    let received = recv_whole_message(
        parts,
        native_parts,
        flags,
        |buffer, capacity, count, recv_flags| {
            let mut source_rid_ptr = ptr::null();
            let rc = unsafe {
                ffi::zlink_recv(
                    handle,
                    &mut source_rid_ptr,
                    buffer,
                    capacity,
                    count,
                    recv_flags,
                )
            };
            if rc == 0 {
                routing_id = routing_id_from_ptr(source_rid_ptr);
            }
            rc
        },
    )?;
    Ok(received.then_some(routing_id))
}

pub(crate) fn recv_subscribed_parts(
    handle: *mut c_void,
    topic_buf: &mut [i8; 256],
    flags: ffi::zlink_recv_flags_t,
    parts: &mut Vec<Message>,
    native_parts: &mut Vec<ffi::zlink_msg_t>,
) -> RecvSubscribedParts {
    let mut routing_id = None;
    let mut topic = smol_str::SmolStr::default();
    let received = recv_whole_message(
        parts,
        native_parts,
        flags,
        |buffer, capacity, count, recv_flags| {
            let mut source_rid_ptr = ptr::null();
            let mut topic_len = topic_buf.len();
            let rc = unsafe {
                ffi::zlink_subscribe(
                    handle,
                    &mut source_rid_ptr,
                    topic_buf.as_mut_ptr(),
                    topic_buf.len(),
                    &mut topic_len,
                    buffer,
                    capacity,
                    count,
                    recv_flags,
                )
            };
            if rc == 0 {
                routing_id = routing_id_from_ptr(source_rid_ptr);
                topic = cstr_buf_to_smolstr(topic_buf, topic_len);
            }
            rc
        },
    )?;
    Ok(received.then_some((routing_id, topic)))
}
