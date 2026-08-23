// SPDX-License-Identifier: MPL-2.0

//! Generic multipart operation storage for the raw Core 0.13.0 socket surface.
//!
//! The operation builders keep message ownership and callback lifetime in this
//! module. Socket contracts only select a raw operation kind; they do not
//! carry routing details into the submit path.

mod reply_ops;
mod send_ops;

pub(crate) use reply_ops::{router_reply_op, submit_reply};
pub(crate) use routed_async::{
    dealer_request_op, router_request_op, submit_routed_request, submit_routed_send,
};
pub(crate) use send_ops::{
    dealer_routed_send_op, immediate_routed_send_op, socket_publish_op, socket_routed_send_op,
    socket_send_op, stream_send_to_op, submit_send,
};

mod routed_async;

pub(crate) fn fixed_topic_or_panic(value: &str, label: &str) -> smol_str::SmolStr {
    assert!(value.len() <= 255, "invalid {label}");
    assert!(!value.as_bytes().contains(&0), "invalid {label}");
    smol_str::SmolStr::new(value)
}
