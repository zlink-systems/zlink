// SPDX-License-Identifier: MPL-2.0

//! Generic multipart operation storage for the raw Core 0.17.0 socket surface.
//!
//! The operation builders keep message and completion ownership in this module.
//! Socket contracts only select a raw operation kind; they do not carry routing
//! details into the submit path.

mod reply_ops;
mod send_ops;

mod routed_async;

pub(crate) fn topic_or_panic(value: &str, label: &str) -> smol_str::SmolStr {
    assert!(!value.as_bytes().contains(&0), "invalid {label}");
    smol_str::SmolStr::new(value)
}
