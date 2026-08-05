// SPDX-License-Identifier: MPL-2.0

//! Generic multipart operation storage for the raw Core 11 socket surface.
//!
//! The operation builders keep message ownership and callback lifetime in this
//! module. Socket contracts only select a raw operation kind; they do not
//! carry routing details into the submit path.

use std::ffi::c_void;

use crate::error::{RequestError, RequestResult};
use crate::ffi;
use crate::message::Message;

mod reply_ops;
mod request_ops;
mod send_ops;

pub(crate) use reply_ops::{router_reply_op, submit_reply};
pub(crate) use request_ops::{dealer_request_op, router_request_op, submit_request};
pub(crate) use send_ops::{socket_publish_op, socket_send_op, socket_send_to_op, submit_send};

pub(crate) type RequestCallback = dyn FnOnce(Result<Vec<Message>, RequestError>) + Send + 'static;

pub(crate) fn fixed_topic_or_panic(value: &str, label: &str) -> smol_str::SmolStr {
    assert!(value.len() <= 255, "invalid {label}");
    assert!(!value.as_bytes().contains(&0), "invalid {label}");
    smol_str::SmolStr::new(value)
}

pub(crate) struct ReplyCallbackState {
    pub(crate) callback: Option<Box<RequestCallback>>,
}

fn request_result_from_native(result: ffi::zlink_request_result_t) -> RequestResult {
    match result {
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_OK => RequestResult::Ok,
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_TIMED_OUT => RequestResult::TimedOut,
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_NOT_FOUND => RequestResult::NotFound,
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_TERMINATED => RequestResult::Terminated,
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_PROTOCOL_ERROR => {
            RequestResult::ProtocolError
        }
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_INTERNAL_ERROR => {
            RequestResult::InternalError
        }
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_REJECTED => RequestResult::Rejected,
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_CONFLICT => RequestResult::Conflict,
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_BUSY => RequestResult::Busy,
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_NOT_CONNECTED => {
            RequestResult::NotConnected
        }
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_INVALID_ARGUMENT => {
            RequestResult::InvalidArgument
        }
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_INVALID_STATE => {
            RequestResult::InvalidState
        }
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_NOT_SUPPORTED => {
            RequestResult::NotSupported
        }
    }
}

pub(crate) unsafe extern "C" fn reply_callback(
    result: ffi::zlink_request_result_t,
    parts: *mut ffi::zlink_msg_t,
    part_count: usize,
    userdata: *mut c_void,
) {
    if userdata.is_null() {
        return;
    }

    // The native callback owns the state pointer after submit succeeds. Taking
    // it exactly once makes the callback terminal.
    let mut state = unsafe { Box::from_raw(userdata as *mut ReplyCallbackState) };
    let received = crate::socket::take_parts(parts, part_count);
    let outcome = if matches!(result, ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_OK) {
        Ok(received)
    } else {
        Err(crate::native_errors::request_error_from_result(
            request_result_from_native(result),
        ))
    };

    if let Some(callback) = state.callback.take() {
        callback(outcome);
    }
}
