use std::ffi::c_void;
use std::os::fd::RawFd;
use std::sync::{Arc, Weak};

use crate::error::{ConfigError, HandlerError, RecvError};
use crate::ffi;
use crate::internal::{CallbackBox, PollerStorage, TimerStorage};
use crate::native_errors::{check_config_rc, check_handler_rc, check_recv_rc, last_errno};
use crate::poller_contracts::{
    POLLCOMPLETION, PollEvent, PollItem, PollSourceKind, Pollable, Poller, Timer,
};

pub(crate) fn poller_new() -> Result<Poller, ConfigError> {
    let handle = unsafe { ffi::zlink_poller_new() };
    if handle.is_null() {
        return Err(crate::error::ConfigError::new(
            crate::error::ConfigResult::InvalidArgument,
            last_errno(),
        ));
    }
    Ok(Poller {
        inner: Box::new(PollerStorage {
            handle,
            sockets: std::sync::Mutex::new(std::collections::HashMap::new()),
            raw_events: std::cell::UnsafeCell::new(Vec::new()),
        }),
    })
}

impl PollerStorage {
    pub(crate) fn add_socket(
        &self,
        socket: &dyn Pollable,
        events: i16,
        slot: usize,
    ) -> Result<(), ConfigError> {
        let handle = pollable_handle(socket)?;
        check_config_rc(unsafe {
            ffi::zlink_poller_add(self.handle, handle, slot as *mut c_void, events)
        })?;
        self.sockets
            .lock()
            .expect("poller sockets")
            .insert(handle as usize, events);
        Ok(())
    }

    /// Modify a regular readiness mask without changing completion ownership.
    pub(crate) fn modify_socket(
        &self,
        socket: &dyn Pollable,
        events: i16,
    ) -> Result<(), ConfigError> {
        let handle = pollable_handle(socket)?;
        let previous = self
            .sockets
            .lock()
            .expect("poller sockets")
            .get(&(handle as usize))
            .copied()
            .unwrap_or(0);
        if previous & POLLCOMPLETION != 0 || events & POLLCOMPLETION != 0 {
            return Err(ConfigError::new(
                crate::error::ConfigResult::InvalidArgument,
                libc::EINVAL,
            ));
        }
        check_config_rc(unsafe { ffi::zlink_poller_modify(self.handle, handle, events) })?;
        self.sockets
            .lock()
            .expect("poller sockets")
            .insert(handle as usize, events);
        Ok(())
    }

    /// Remove a socket from the poller.
    pub(crate) fn remove_socket(&self, socket: &dyn Pollable) -> Result<(), ConfigError> {
        let handle = pollable_handle(socket)?;
        check_config_rc(unsafe { ffi::zlink_poller_remove(self.handle, handle) })?;
        self.sockets
            .lock()
            .expect("poller sockets")
            .remove(&(handle as usize));
        Ok(())
    }

    pub(crate) fn add_fd(&self, fd: RawFd, events: i16, slot: usize) -> Result<(), ConfigError> {
        check_config_rc(unsafe {
            ffi::zlink_poller_add_fd(
                self.handle,
                fd as ffi::zlink_fd_t,
                slot as *mut c_void,
                events,
            )
        })
    }

    pub(crate) fn modify_fd(&self, fd: RawFd, events: i16) -> Result<(), ConfigError> {
        check_config_rc(unsafe {
            ffi::zlink_poller_modify_fd(self.handle, fd as ffi::zlink_fd_t, events)
        })
    }

    pub(crate) fn remove_fd(&self, fd: RawFd) -> Result<(), ConfigError> {
        check_config_rc(unsafe { ffi::zlink_poller_remove_fd(self.handle, fd as ffi::zlink_fd_t) })
    }

    pub(crate) fn add_timer(&self, timer: &Timer, slot: usize) -> Result<(), ConfigError> {
        check_config_rc(unsafe {
            ffi::zlink_poller_add_timer(
                self.handle,
                timer_native_handle(timer),
                slot as *mut c_void,
            )
        })
    }

    pub(crate) fn remove_timer(&self, timer: &Timer) -> Result<(), ConfigError> {
        check_config_rc(unsafe {
            ffi::zlink_poller_remove_timer(self.handle, timer_native_handle(timer))
        })
    }

    /// Wait for events on any registered source.
    ///
    /// `timeout_ms`: -1 = block indefinitely, 0 = return immediately,
    /// positive = timeout in milliseconds.
    ///
    /// Returns the number of entries written to `events`.
    pub(crate) fn wait(
        &self,
        events: &mut [PollEvent],
        timeout_ms: i64,
    ) -> Result<usize, RecvError> {
        if events.is_empty() {
            return Err(crate::error::RecvError::new(
                crate::error::RecvResult::InternalError,
                libc::EINVAL,
            ));
        }
        let raw_events = unsafe { &mut *self.raw_events.get() };
        if raw_events.len() < events.len() {
            raw_events.resize(
                events.len(),
                ffi::zlink_poller_event_t {
                    source_kind: ffi::zlink_poller_source_kind_t::ZLINK_POLLER_SOURCE_SOCKET,
                    socket: std::ptr::null_mut(),
                    fd: 0,
                    timer: std::ptr::null_mut(),
                    user_data: std::ptr::null_mut(),
                    events: 0,
                },
            );
        }
        let rc = unsafe {
            ffi::zlink_poller_wait(
                self.handle,
                raw_events.as_mut_ptr(),
                events.len() as i32,
                timeout_ms as std::ffi::c_long,
                std::ptr::null_mut(),
            )
        };
        if rc < 0 {
            let errno = last_errno();
            if errno == libc::EAGAIN || errno == libc::ETIMEDOUT {
                return Ok(0);
            }
            return Err(crate::error::RecvError::new(
                crate::error::RecvResult::Terminated,
                errno,
            ));
        }
        if rc == 0 {
            return Ok(0);
        }
        for (dst, src) in events.iter_mut().zip(raw_events.iter()).take(rc as usize) {
            *dst = PollEvent {
                source_kind: match src.source_kind {
                    ffi::zlink_poller_source_kind_t::ZLINK_POLLER_SOURCE_FD => PollSourceKind::Fd,
                    ffi::zlink_poller_source_kind_t::ZLINK_POLLER_SOURCE_TIMER => {
                        PollSourceKind::Timer
                    }
                    _ => PollSourceKind::Socket,
                },
                fd: src.fd as RawFd,
                slot: src.user_data as usize,
                revents: src.events,
            };
        }
        Ok(rc as usize)
    }

    pub(crate) fn size(&self) -> i32 {
        let mut error_out = 0;
        unsafe { ffi::zlink_poller_size(self.handle, &mut error_out) }
    }
}

impl Drop for PollerStorage {
    fn drop(&mut self) {
        unsafe {
            let mut h = self.handle;
            ffi::zlink_poller_destroy(&mut h);
        }
    }
}

pub fn poll(items: &mut [PollItem], timeout_ms: i64) -> Result<i32, RecvError> {
    const INLINE_POLL_ITEMS: usize = 16;
    let mut inline = [ffi::zlink_pollitem_t {
        socket: std::ptr::null_mut(),
        fd: 0,
        events: 0,
        revents: 0,
    }; INLINE_POLL_ITEMS];
    let mut heap = Vec::new();
    let raw: &mut [ffi::zlink_pollitem_t] = if items.len() <= INLINE_POLL_ITEMS {
        for (dst, src) in inline.iter_mut().zip(items.iter()) {
            *dst = ffi::zlink_pollitem_t {
                socket: std::ptr::null_mut(),
                fd: src.fd as ffi::zlink_fd_t,
                events: src.events,
                revents: src.revents,
            };
        }
        &mut inline[..items.len()]
    } else {
        heap.extend(items.iter().map(|item| ffi::zlink_pollitem_t {
            socket: std::ptr::null_mut(),
            fd: item.fd as ffi::zlink_fd_t,
            events: item.events,
            revents: item.revents,
        }));
        heap.as_mut_slice()
    };
    let rc = unsafe {
        ffi::zlink_poll(
            raw.as_mut_ptr(),
            raw.len() as i32,
            timeout_ms as std::ffi::c_long,
            std::ptr::null_mut(),
        )
    };
    if rc < 0 {
        return Err(crate::error::RecvError::new(
            crate::error::RecvResult::Terminated,
            last_errno(),
        ));
    }
    for (idx, item) in items.iter_mut().enumerate() {
        item.revents = raw[idx].revents;
    }
    Ok(rc)
}

pub(crate) fn timer_new() -> Result<Timer, ConfigError> {
    let handle = unsafe { ffi::zlink_timer_new() };
    if handle.is_null() {
        return Err(crate::error::ConfigError::new(
            crate::error::ConfigResult::InvalidArgument,
            last_errno(),
        ));
    }
    Ok(Timer {
        inner: Arc::new(TimerStorage {
            handle,
            callback: std::sync::Mutex::new(None),
        }),
    })
}

impl TimerStorage {
    pub(crate) fn start(&self, interval_ns: u64, repeat_count: u64) -> Result<(), ConfigError> {
        check_config_rc(unsafe { ffi::zlink_timer_start(self.handle, interval_ns, repeat_count) })
    }

    pub(crate) fn stop(&self) -> Result<(), ConfigError> {
        check_config_rc(unsafe { ffi::zlink_timer_stop(self.handle) })
    }

    /// Receive a timer fire count. Returns `Ok(None)` when no data is available (EAGAIN).
    pub(crate) fn recv(&self) -> Result<Option<u64>, RecvError> {
        let mut count = 0u64;
        let rc = unsafe { ffi::zlink_timer_recv(self.handle, &mut count) };
        if rc != 0 {
            let errno = last_errno();
            if errno == libc::EAGAIN {
                return Ok(None);
            }
            return Err(check_recv_rc(rc).unwrap_err());
        }
        Ok(Some(count))
    }

    pub(crate) fn on_fire<F>(
        &self,
        timer: Weak<TimerStorage>,
        handler: F,
    ) -> Result<(), HandlerError>
    where
        F: Fn(&Timer, u64) + Send + 'static,
    {
        let (cb, userdata) = CallbackBox::new(TimerCallback { timer, handler });
        let mut callback = self.callback.lock().expect("timer callback");
        if let Some(previous) = callback.as_ref() {
            previous.set_closing(true);
        }
        let rc = unsafe { ffi::zlink_timer_handler(self.handle, timer_trampoline::<F>, userdata) };
        if rc != 0 {
            if let Some(previous) = callback.as_ref() {
                previous.set_closing(false);
            }
            drop(cb);
            return Err(check_handler_rc(rc).unwrap_err());
        }
        let previous = callback.replace(cb);
        drop(callback);
        if let Some(previous) = previous {
            crate::internal::release_callbacks(vec![previous]);
        }
        Ok(())
    }
}

struct TimerCallback<F> {
    timer: Weak<TimerStorage>,
    handler: F,
}

impl<F: Fn(&Timer, u64)> TimerCallback<F> {
    fn invoke(&self, count: u64) {
        let Some(inner) = self.timer.upgrade() else {
            return;
        };
        let timer = Timer { inner };
        (self.handler)(&timer, count);
    }
}

unsafe extern "C" fn timer_trampoline<F: Fn(&Timer, u64) + Send + 'static>(
    _timer: *mut c_void,
    count: u64,
    userdata: *mut c_void,
) {
    let _ = unsafe {
        CallbackBox::invoke::<TimerCallback<F>, _>(userdata, |callback| {
            callback.invoke(count);
        })
    };
}

impl Drop for TimerStorage {
    fn drop(&mut self) {
        let mut callback_slot = self.callback.lock().expect("timer callback");
        if let Some(callback) = callback_slot.as_ref() {
            callback.set_closing(true);
        }
        let callback = callback_slot.take().into_iter().collect();
        drop(callback_slot);
        let mut handle = self.handle;
        let rc = unsafe { ffi::zlink_timer_destroy(&mut handle) };
        if rc == 0 {
            crate::internal::release_callbacks(callback);
        } else {
            crate::internal::defer_native_close(
                crate::internal::DeferredCloseKind::Timer,
                self.handle,
                callback,
            );
        }
    }
}

fn timer_native_handle(timer: &Timer) -> *mut c_void {
    timer.inner.handle
}

pub(crate) fn pollable_handle(source: &dyn Pollable) -> Result<*mut c_void, ConfigError> {
    let any = source.as_any();
    if let Some(socket) = any.downcast_ref::<crate::PairSocket>() {
        return Ok(crate::socket::pair_handle(socket));
    }
    macro_rules! downcast_socket {
        ($ty:ident, $inner:path) => {
            if let Some(socket) = any.downcast_ref::<crate::$ty>() {
                return Ok($inner(socket).handle);
            }
        };
    }
    downcast_socket!(PubSocket, crate::socket::pub_inner);
    downcast_socket!(SubSocket, crate::socket::sub_inner);
    downcast_socket!(DealerSocket, crate::socket::dealer_inner);
    downcast_socket!(RouterSocket, crate::socket::router_inner);
    downcast_socket!(XPubSocket, crate::socket::xpub_inner);
    downcast_socket!(XSubSocket, crate::socket::xsub_inner);
    downcast_socket!(StreamSocket, crate::socket::stream_inner);
    Err(ConfigError::new(
        crate::error::ConfigResult::InvalidArgument,
        libc::EINVAL,
    ))
}

macro_rules! impl_pollable {
    ($ty:ident) => {
        impl crate::poller_contracts::private::Sealed for crate::$ty {}

        impl Pollable for crate::$ty {
            fn as_any(&self) -> &dyn std::any::Any {
                self
            }
        }
    };
}

impl_pollable!(PubSocket);
impl_pollable!(SubSocket);
impl_pollable!(DealerSocket);
impl_pollable!(RouterSocket);
impl_pollable!(XPubSocket);
impl_pollable!(XSubSocket);
impl_pollable!(StreamSocket);

impl crate::poller_contracts::private::Sealed for crate::PairSocket {}

impl Pollable for crate::PairSocket {
    fn as_any(&self) -> &dyn std::any::Any {
        self
    }
}
