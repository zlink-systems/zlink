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

/// Refill the envelope's private scratch storage in place. The public parts
/// are swapped only after FINAL, so NO_DATA and errors preserve the envelope.
/// Core closes the previous contents on a successful receive; initialized
/// headers can therefore be reused without a temporary message and move.
pub(crate) fn recv_part_sequence(
    parts: &mut Vec<Message>,
    flags: ffi::zlink_recv_flags_t,
    mut receive: impl FnMut(
        *mut ffi::zlink_msg_t,
        *mut ffi::zlink_part_flag_t,
        ffi::zlink_recv_flags_t,
        bool,
    ) -> i32,
) -> Result<bool, RecvError> {
    let mut count = 0;
    let mut recv_flags = flags;
    loop {
        if count == parts.len() {
            parts.push(Message::new().map_err(|error| {
                RecvError::new(RecvResult::InternalError, error.native_errno())
            })?);
        }
        let mut has_more = ffi::zlink_part_flag_t::ZLINK_PART_FINAL;
        let rc = receive(
            parts[count].raw_mut(),
            &mut has_more,
            recv_flags,
            count == 0,
        );
        if rc != 0 {
            if count == 0
                && (rc == RecvResult::NoData as i32
                    || unsafe { ffi::zlink_errno() } == libc::EAGAIN)
            {
                return Ok(false);
            }
            return Err(check_recv_rc(rc).unwrap_err());
        }
        count += 1;
        if has_more == ffi::zlink_part_flag_t::ZLINK_PART_FINAL {
            parts.truncate(count);
            return Ok(true);
        }
        recv_flags = ffi::ZLINK_DONTWAIT;
    }
}

pub(crate) fn recv_basic_parts(
    handle: *mut c_void,
    flags: ffi::zlink_recv_flags_t,
    parts: &mut Vec<Message>,
) -> RecvBasicParts {
    let mut routing_id = None;
    let received = recv_part_sequence(parts, flags, |part, has_more, recv_flags, first| {
        let mut source_rid_ptr = ptr::null();
        let rc = unsafe {
            ffi::zlink_recv_part(handle, &mut source_rid_ptr, part, has_more, recv_flags)
        };
        if first && rc == 0 {
            routing_id = routing_id_from_ptr(source_rid_ptr);
        }
        rc
    })?;
    Ok(received.then_some(routing_id))
}

pub(crate) fn recv_subscribed_parts(
    handle: *mut c_void,
    topic_buf: &mut [i8; 256],
    flags: ffi::zlink_recv_flags_t,
    parts: &mut Vec<Message>,
) -> RecvSubscribedParts {
    let mut routing_id = None;
    let mut topic = smol_str::SmolStr::default();
    let received = recv_part_sequence(parts, flags, |part, has_more, recv_flags, first| {
        let mut source_rid_ptr = ptr::null();
        let mut topic_len = topic_buf.len();
        let rc = unsafe {
            ffi::zlink_subscribe_part(
                handle,
                &mut source_rid_ptr,
                topic_buf.as_mut_ptr(),
                topic_buf.len(),
                &mut topic_len,
                part,
                has_more,
                recv_flags,
            )
        };
        if first && rc == 0 {
            routing_id = routing_id_from_ptr(source_rid_ptr);
            topic = cstr_buf_to_smolstr(topic_buf, topic_len);
        }
        rc
    })?;
    Ok(received.then_some((routing_id, topic)))
}
