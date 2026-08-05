// SPDX-License-Identifier: MPL-2.0
//
// ABI layout sanity tests. The Rust bindings keep #[repr(C)] mirrors of
// several core C structs. When the core layout changes (or when the Rust
// mirror drifts), zlink_poller_wait() and friends will write the C struct
// shape into a smaller / differently-aligned Rust struct, silently corrupting
// the stack. These tests pin the expected sizes/offsets so any drift fails the
// rust test suite instead of crashing user code.

use std::mem::{align_of, offset_of, size_of};

#[test]
fn poller_event_layout_matches_c_abi() {
    // Lift the mirror into scope without dragging in private modules: it
    // is part of the crate's runtime/native bridge and is repr(C).
    use zlink_ffi_check::ZlinkPollerEvent;

    // C struct layout (see core/include/zlink/monitoring.h):
    //   zlink_poller_source_kind_t source_kind;   // 4 bytes (enum, int)
    //   void *socket;                              // 8 bytes (pointer)
    //   zlink_fd_t fd;                             // 8 bytes (intptr_t)
    //   void *timer;                               // 8 bytes
    //   void *user_data;                           // 8 bytes
    //   short events;                              // 2 bytes
    // Total = 40 with 4-byte alignment after source_kind, padding to 8 before pointer,
    //         2-byte tail with trailing pad to 8 -> 48 bytes total on 64-bit.
    assert_eq!(
        size_of::<ZlinkPollerEvent>(),
        48,
        "zlink_poller_event_t size drifted from the C ABI"
    );
    assert_eq!(align_of::<ZlinkPollerEvent>(), 8);
    assert_eq!(offset_of!(ZlinkPollerEvent, source_kind), 0);
    assert_eq!(offset_of!(ZlinkPollerEvent, socket), 8);
    assert_eq!(offset_of!(ZlinkPollerEvent, fd), 16);
    assert_eq!(offset_of!(ZlinkPollerEvent, timer), 24);
    assert_eq!(offset_of!(ZlinkPollerEvent, user_data), 32);
    assert_eq!(offset_of!(ZlinkPollerEvent, events), 40);
}

/// Mirror exposed for ABI tests only. Kept here so we don't have to publish
/// the FFI module from `lib.rs`.
mod zlink_ffi_check {
    use std::os::raw::c_void;

    #[repr(C)]
    #[allow(dead_code)]
    pub enum ZlinkPollerSourceKind {
        Socket = 1,
        Fd = 2,
        Timer = 3,
    }

    #[repr(C)]
    pub struct ZlinkPollerEvent {
        pub source_kind: ZlinkPollerSourceKind,
        pub socket: *mut c_void,
        pub fd: isize,
        pub timer: *mut c_void,
        pub user_data: *mut c_void,
        pub events: i16,
    }
}
