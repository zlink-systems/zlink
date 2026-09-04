use super::*;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Submit message parts directly from their owned `Message` storage.
///
/// Core consumes each `zlink_msg_t` on every result, including DONTWAIT
/// backpressure. Callers that may retry must therefore submit cloned attempt
/// records and retain the logical packet separately.
pub(crate) fn submit_part_sequence(
    parts: &mut crate::messaging_operations::MessageParts,
    mut submit: impl FnMut(*mut ffi::zlink_msg_t, ffi::zlink_part_flag_t, bool) -> i32,
) -> Result<i32, SubmitError> {
    if parts.is_empty() {
        return Err(submit_validation_error());
    }

    let part_count = parts.len();
    for (index, part) in parts.iter_mut().enumerate() {
        let is_final = index + 1 == part_count;
        let part_flag = if is_final {
            ffi::zlink_part_flag_t::ZLINK_PART_FINAL
        } else {
            ffi::zlink_part_flag_t::ZLINK_PART_MORE
        };
        let rc = submit(part.raw_mut(), part_flag, is_final);
        if rc != 0 {
            return Ok(rc);
        }
    }

    Ok(0)
}

pub(crate) fn close_unreceived_part(part: &mut MaybeUninit<ffi::zlink_msg_t>) {
    unsafe {
        ffi::zlink_msg_close(part.as_mut_ptr());
    }
}

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

pub(crate) fn recv_basic_parts(
    handle: *mut c_void,
    flags: ffi::zlink_recv_flags_t,
    parts: &mut Vec<Message>,
) -> RecvBasicParts {
    let mut routing_id = None;
    let mut recv_flags = flags;
    let mut received_any = false;

    loop {
        let mut source_rid_ptr = ptr::null();
        let mut part = MaybeUninit::<ffi::zlink_msg_t>::uninit();
        unsafe {
            ffi::zlink_msg_init(part.as_mut_ptr());
        }
        let mut has_more = ffi::zlink_part_flag_t::ZLINK_PART_FINAL;
        let rc = unsafe {
            ffi::zlink_recv_part(
                handle,
                &mut source_rid_ptr,
                part.as_mut_ptr(),
                &mut has_more,
                recv_flags,
            )
        };

        if !received_any {
            if rc == RecvResult::NoData as i32 {
                close_unreceived_part(&mut part);
                return Ok(None);
            }
            if rc != 0 {
                close_unreceived_part(&mut part);
                let errno = unsafe { ffi::zlink_errno() };
                if errno == libc::EAGAIN {
                    return Ok(None);
                }
                return Err(check_recv_rc(rc).unwrap_err());
            }
            routing_id = routing_id_from_ptr(source_rid_ptr);
            parts.clear();
            received_any = true;
        } else if rc != 0 {
            close_unreceived_part(&mut part);
            return Err(check_recv_rc(rc).unwrap_err());
        }

        parts.push(unsafe { Message::from_raw(part.assume_init()) });
        if has_more == ffi::zlink_part_flag_t::ZLINK_PART_FINAL {
            return Ok(Some(routing_id));
        }
        recv_flags = ffi::ZLINK_DONTWAIT;
    }
}

pub(crate) fn recv_subscribed_parts(
    handle: *mut c_void,
    topic_buf: &mut [i8; 256],
    flags: ffi::zlink_recv_flags_t,
    parts: &mut Vec<Message>,
) -> RecvSubscribedParts {
    let mut routing_id = None;
    let mut topic = smol_str::SmolStr::default();
    let mut recv_flags = flags;
    let mut received_any = false;

    loop {
        let mut source_rid_ptr = ptr::null();
        let mut topic_len = topic_buf.len();
        let mut part = MaybeUninit::<ffi::zlink_msg_t>::uninit();
        unsafe {
            ffi::zlink_msg_init(part.as_mut_ptr());
        }
        let mut has_more = ffi::zlink_part_flag_t::ZLINK_PART_FINAL;
        let rc = unsafe {
            ffi::zlink_subscribe_part(
                handle,
                &mut source_rid_ptr,
                topic_buf.as_mut_ptr(),
                topic_buf.len(),
                &mut topic_len,
                part.as_mut_ptr(),
                &mut has_more,
                recv_flags,
            )
        };

        if !received_any {
            if rc == RecvResult::NoData as i32 {
                close_unreceived_part(&mut part);
                return Ok(None);
            }
            if rc != 0 {
                close_unreceived_part(&mut part);
                let errno = unsafe { ffi::zlink_errno() };
                if errno == libc::EAGAIN {
                    return Ok(None);
                }
                return Err(check_recv_rc(rc).unwrap_err());
            }
            routing_id = routing_id_from_ptr(source_rid_ptr);
            topic = cstr_buf_to_smolstr(topic_buf, topic_len);
            parts.clear();
            received_any = true;
        } else if rc != 0 {
            close_unreceived_part(&mut part);
            return Err(check_recv_rc(rc).unwrap_err());
        }

        parts.push(unsafe { Message::from_raw(part.assume_init()) });
        if has_more == ffi::zlink_part_flag_t::ZLINK_PART_FINAL {
            return Ok(Some((routing_id, topic)));
        }
        recv_flags = ffi::ZLINK_DONTWAIT;
    }
}
