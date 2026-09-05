use std::ffi::c_void;
#[cfg(unix)]
use std::os::fd::RawFd;
#[cfg(windows)]
use std::os::windows::io::RawSocket as RawFd;
use std::sync::Arc;

use crate::SocketMonitor;
use crate::error::{ConfigError, RecvError};
use crate::ffi;
use crate::internal::{PollerSocketRegistration, PollerStorage, TimerStorage};
use crate::native_errors::{check_config_rc, check_recv_rc, last_errno};
use crate::poller_contracts::{
    POLLCOMPLETION, POLLIN, PollEvent, PollItem, PollSourceKind, Pollable, Poller, Timer,
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
        let (handle, completion_owner) = pollable_registration(socket)?;
        let owner = self as *const Self as usize;
        if events & POLLCOMPLETION != 0 {
            let completion_owner = completion_owner.as_ref().ok_or_else(|| {
                ConfigError::new(crate::ConfigResult::InvalidArgument, libc::EINVAL)
            })?;
            completion_owner.transfer_to_public(owner)?;
        }
        if let Err(error) = check_config_rc(unsafe {
            ffi::zlink_poller_add(self.handle, handle, slot as *mut c_void, events)
        }) {
            if events & POLLCOMPLETION != 0 {
                completion_owner
                    .as_ref()
                    .expect("completion owner")
                    .transfer_to_runtime(owner);
            }
            return Err(error);
        }
        self.sockets.lock().expect("poller sockets").insert(
            handle as usize,
            PollerSocketRegistration {
                events,
                completion_owner,
            },
        );
        Ok(())
    }

    /// Modify readiness and atomically transfer completion ownership as needed.
    pub(crate) fn modify_socket(
        &self,
        socket: &dyn Pollable,
        events: i16,
    ) -> Result<(), ConfigError> {
        let (handle, completion_owner) = pollable_registration(socket)?;
        let previous = self
            .sockets
            .lock()
            .expect("poller sockets")
            .get(&(handle as usize))
            .cloned();
        let previous = previous
            .ok_or_else(|| ConfigError::new(crate::ConfigResult::InvalidArgument, libc::EINVAL))?;
        let had_completion = previous.events & POLLCOMPLETION != 0;
        let wants_completion = events & POLLCOMPLETION != 0;
        let owner = self as *const Self as usize;
        if !had_completion && wants_completion {
            let completion_owner = completion_owner
                .as_ref()
                .ok_or_else(|| ConfigError::new(crate::ConfigResult::InvalidState, libc::EINVAL))?;
            completion_owner.transfer_to_public(owner)?;
        }
        if let Err(error) =
            check_config_rc(unsafe { ffi::zlink_poller_modify(self.handle, handle, events) })
        {
            if !had_completion && wants_completion {
                completion_owner
                    .as_ref()
                    .expect("completion owner")
                    .transfer_to_runtime(owner);
            }
            return Err(error);
        }
        self.sockets.lock().expect("poller sockets").insert(
            handle as usize,
            PollerSocketRegistration {
                events,
                completion_owner,
            },
        );
        if had_completion && !wants_completion {
            previous
                .completion_owner
                .expect("completion owner")
                .transfer_to_runtime(owner);
        }
        Ok(())
    }

    /// Remove a socket from the poller.
    pub(crate) fn remove_socket(&self, socket: &dyn Pollable) -> Result<(), ConfigError> {
        let (handle, _) = pollable_registration(socket)?;
        let registration = self
            .sockets
            .lock()
            .expect("poller sockets")
            .get(&(handle as usize))
            .cloned();
        check_config_rc(unsafe { ffi::zlink_poller_remove(self.handle, handle) })?;
        self.sockets
            .lock()
            .expect("poller sockets")
            .remove(&(handle as usize));
        if let Some(registration) = registration {
            if registration.events & POLLCOMPLETION != 0 {
                if let Some(owner) = registration.completion_owner {
                    owner.transfer_to_runtime(self as *const Self as usize);
                }
            }
        }
        Ok(())
    }

    pub(crate) fn add_monitor(
        &self,
        monitor: &SocketMonitor,
        events: i16,
        slot: usize,
    ) -> Result<(), ConfigError> {
        validate_monitor_events(events)?;
        check_config_rc(unsafe {
            ffi::zlink_poller_add(
                self.handle,
                monitor_native_handle(monitor),
                slot as *mut c_void,
                events,
            )
        })
    }

    pub(crate) fn modify_monitor(
        &self,
        monitor: &SocketMonitor,
        events: i16,
    ) -> Result<(), ConfigError> {
        validate_monitor_events(events)?;
        check_config_rc(unsafe {
            ffi::zlink_poller_modify(self.handle, monitor_native_handle(monitor), events)
        })
    }

    pub(crate) fn remove_monitor(&self, monitor: &SocketMonitor) -> Result<(), ConfigError> {
        check_config_rc(unsafe {
            ffi::zlink_poller_remove(self.handle, monitor_native_handle(monitor))
        })
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
        let registrations = self.sockets.lock().expect("poller sockets");
        let mut written = 0usize;
        for src in raw_events.iter().take(rc as usize) {
            let mut revents = src.events;
            let registration = registrations.get(&(src.socket as usize));
            if revents & (crate::POLLOUT | POLLCOMPLETION) != 0
                && registration.is_some_and(|item| item.events & POLLCOMPLETION != 0)
            {
                let drained = registration
                    .and_then(|item| item.completion_owner.as_ref())
                    .map(|owner| owner.drain(true))
                    .transpose()?
                    .unwrap_or(0);
                if drained == 0 {
                    revents &= !POLLCOMPLETION;
                }
            }
            if revents == 0 {
                continue;
            }
            events[written] = PollEvent {
                source_kind: match src.source_kind {
                    ffi::zlink_poller_source_kind_t::ZLINK_POLLER_SOURCE_FD => PollSourceKind::Fd,
                    ffi::zlink_poller_source_kind_t::ZLINK_POLLER_SOURCE_TIMER => {
                        PollSourceKind::Timer
                    }
                    _ => PollSourceKind::Socket,
                },
                fd: src.fd as RawFd,
                slot: src.user_data as usize,
                revents,
            };
            written += 1;
        }
        Ok(written)
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
        let owner = self as *const Self as usize;
        let registrations = self.sockets.get_mut().expect("poller sockets");
        for registration in registrations.drain().map(|(_, value)| value) {
            if registration.events & POLLCOMPLETION != 0 {
                if let Some(completion_owner) = registration.completion_owner {
                    completion_owner.transfer_to_runtime(owner);
                }
            }
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
        inner: Arc::new(TimerStorage { handle }),
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
}

impl Drop for TimerStorage {
    fn drop(&mut self) {
        let mut handle = self.handle;
        let rc = unsafe { ffi::zlink_timer_destroy(&mut handle) };
        if rc != 0 {
            crate::internal::defer_native_close(
                crate::internal::DeferredCloseKind::Timer,
                self.handle,
            );
        }
    }
}

fn timer_native_handle(timer: &Timer) -> *mut c_void {
    timer.inner.handle
}

fn monitor_native_handle(monitor: &SocketMonitor) -> *mut c_void {
    monitor.inner.handle
}

fn validate_monitor_events(events: i16) -> Result<(), ConfigError> {
    if events != POLLIN {
        return Err(ConfigError::new(
            crate::error::ConfigResult::InvalidArgument,
            libc::EINVAL,
        ));
    }
    Ok(())
}

pub(crate) fn pollable_handle(source: &dyn Pollable) -> Result<*mut c_void, ConfigError> {
    Ok(pollable_registration(source)?.0)
}

fn pollable_registration(
    source: &dyn Pollable,
) -> Result<(*mut c_void, Option<Arc<crate::internal::CompletionOwner>>), ConfigError> {
    let any = source.as_any();
    if let Some(socket) = any.downcast_ref::<crate::PairSocket>() {
        let inner = crate::socket::pair_inner(socket);
        return Ok((inner.handle, inner.completion_owner.clone()));
    }
    macro_rules! downcast_socket {
        ($ty:ident, $inner:path) => {
            if let Some(socket) = any.downcast_ref::<crate::$ty>() {
                let inner = $inner(socket);
                return Ok((inner.handle, inner.completion_owner.clone()));
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
