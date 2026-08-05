use std::ffi::c_void;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Condvar, Mutex};

pub(crate) struct CallbackBox {
    pub(crate) data: *mut c_void,
    pub(crate) drop_fn: unsafe fn(*mut c_void),
    set_closing_fn: unsafe fn(*mut c_void, bool),
    is_idle_fn: unsafe fn(*mut c_void) -> bool,
    wait_idle_fn: unsafe fn(*mut c_void),
}

unsafe impl Send for CallbackBox {}

impl CallbackBox {
    pub(crate) fn new<F: Send + 'static>(f: F) -> (Self, *mut c_void) {
        let ptr = Box::into_raw(Box::new(CallbackState::new(f)));
        let callback = Self {
            data: ptr as *mut c_void,
            drop_fn: drop_state::<F>,
            set_closing_fn: set_state_closing::<F>,
            is_idle_fn: state_is_idle::<F>,
            wait_idle_fn: state_wait_idle::<F>,
        };
        (callback, ptr as *mut c_void)
    }

    pub(crate) fn set_closing(&self, closing: bool) {
        unsafe { (self.set_closing_fn)(self.data, closing) }
    }

    pub(crate) fn is_idle(&self) -> bool {
        unsafe { (self.is_idle_fn)(self.data) }
    }

    pub(crate) fn wait_idle(&self) {
        unsafe { (self.wait_idle_fn)(self.data) }
    }

    /// Invokes an erased callback while keeping its storage alive until the
    /// invocation returns. The callback mutex keeps the public `Send` bound
    /// sound even when a caller's closure is not `Sync`.
    pub(crate) unsafe fn invoke<F, R>(
        userdata: *mut c_void,
        invoke: impl FnOnce(&F) -> R,
    ) -> Option<R>
    where
        F: Send + 'static,
    {
        if userdata.is_null() {
            return None;
        }
        let state = unsafe { &*(userdata as *const CallbackState<F>) };
        let _active = state.enter()?;
        let result = {
            let callback = state.callback.lock().expect("callback state");
            invoke(&callback)
        };
        Some(result)
    }

    pub(crate) unsafe fn invoke_or<F, R>(
        userdata: *mut c_void,
        invoke: impl FnOnce(&F) -> R,
        fallback: impl FnOnce() -> R,
    ) -> R
    where
        F: Send + 'static,
    {
        unsafe { Self::invoke(userdata, invoke) }.unwrap_or_else(fallback)
    }
}

impl Drop for CallbackBox {
    fn drop(&mut self) {
        self.set_closing(true);
        self.wait_idle();
        unsafe { (self.drop_fn)(self.data) }
    }
}

struct CallbackState<F> {
    callback: Mutex<F>,
    state: AtomicUsize,
    idle: Condvar,
    idle_guard: Mutex<()>,
}

const CALLBACK_CLOSING: usize = 1usize << (usize::BITS - 1);
const CALLBACK_ACTIVE_MASK: usize = !CALLBACK_CLOSING;

impl<F> CallbackState<F> {
    fn new(callback: F) -> Self {
        Self {
            callback: Mutex::new(callback),
            state: AtomicUsize::new(0),
            idle: Condvar::new(),
            idle_guard: Mutex::new(()),
        }
    }

    fn enter(&self) -> Option<CallbackGuard<'_, F>> {
        let mut state = self.state.load(Ordering::Acquire);
        loop {
            if state & CALLBACK_CLOSING != 0 || state & CALLBACK_ACTIVE_MASK == CALLBACK_ACTIVE_MASK
            {
                return None;
            }
            match self.state.compare_exchange_weak(
                state,
                state + 1,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => return Some(CallbackGuard { state: self }),
                Err(next) => state = next,
            }
        }
    }

    fn leave(&self) {
        let previous = self.state.fetch_sub(1, Ordering::AcqRel);
        debug_assert_ne!(previous & CALLBACK_ACTIVE_MASK, 0);
        if previous & CALLBACK_ACTIVE_MASK == 1 {
            self.idle.notify_all();
        }
    }

    fn set_closing(&self, closing: bool) {
        if closing {
            self.state.fetch_or(CALLBACK_CLOSING, Ordering::AcqRel);
        } else {
            self.state
                .fetch_and(CALLBACK_ACTIVE_MASK, Ordering::Release);
            self.idle.notify_all();
        }
    }

    fn is_idle(&self) -> bool {
        self.state.load(Ordering::Acquire) & CALLBACK_ACTIVE_MASK == 0
    }

    fn wait_idle(&self) {
        if self.is_idle() {
            return;
        }
        let mut guard = self.idle_guard.lock().expect("callback idle state");
        while !self.is_idle() {
            guard = self.idle.wait(guard).expect("callback idle wait");
        }
    }
}

struct CallbackGuard<'a, F> {
    state: &'a CallbackState<F>,
}

impl<F> Drop for CallbackGuard<'_, F> {
    fn drop(&mut self) {
        self.state.leave();
    }
}

unsafe fn drop_state<F>(ptr: *mut c_void) {
    drop(unsafe { Box::from_raw(ptr as *mut CallbackState<F>) });
}

unsafe fn set_state_closing<F>(ptr: *mut c_void, closing: bool) {
    unsafe { (*(ptr as *const CallbackState<F>)).set_closing(closing) };
}

unsafe fn state_is_idle<F>(ptr: *mut c_void) -> bool {
    unsafe { (*(ptr as *const CallbackState<F>)).is_idle() }
}

unsafe fn state_wait_idle<F>(ptr: *mut c_void) {
    unsafe { (*(ptr as *const CallbackState<F>)).wait_idle() };
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, Barrier};

    #[test]
    fn callback_close_waits_for_active_invocation() {
        let entered = Arc::new(Barrier::new(2));
        let release = Arc::new(Barrier::new(2));
        let callback: Box<dyn Fn() + Send> = {
            let entered = Arc::clone(&entered);
            let release = Arc::clone(&release);
            Box::new(move || {
                entered.wait();
                release.wait();
            })
        };
        let (callback_box, userdata) = CallbackBox::new(callback);
        let userdata = userdata as usize;
        let join = std::thread::spawn(move || unsafe {
            CallbackBox::invoke::<Box<dyn Fn() + Send>, _>(userdata as *mut c_void, |callback| {
                callback()
            })
        });

        entered.wait();
        callback_box.set_closing(true);
        assert!(!callback_box.is_idle());
        release.wait();
        assert!(join.join().expect("callback thread").is_some());
        assert!(callback_box.is_idle());
        assert!(
            unsafe {
                CallbackBox::invoke::<Box<dyn Fn() + Send>, _>(
                    userdata as *mut c_void,
                    |callback| callback(),
                )
            }
            .is_none()
        );
    }
}
