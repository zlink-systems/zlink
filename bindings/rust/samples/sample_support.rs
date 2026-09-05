#![allow(dead_code)]

use std::future::Future;
use std::pin::pin;
use std::sync::Arc;
use std::task::{Context, Poll, Wake, Waker};
use std::thread::{self, Thread};
use std::time::{Duration, Instant};

use zlink::{POLLIN, PollEvent, Poller, RecvFlags, SocketMonitor};

struct ThreadWake(Thread);

impl Wake for ThreadWake {
    fn wake(self: Arc<Self>) {
        self.0.unpark();
    }

    fn wake_by_ref(self: &Arc<Self>) {
        self.0.unpark();
    }
}

pub(crate) fn block_on<F: Future>(future: F) -> F::Output {
    let waker = Waker::from(Arc::new(ThreadWake(thread::current())));
    let mut context = Context::from_waker(&waker);
    let mut future = pin!(future);
    loop {
        match future.as_mut().poll(&mut context) {
            Poll::Ready(output) => return output,
            Poll::Pending => thread::park(),
        }
    }
}

pub fn tcp_endpoint() -> String {
    let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    drop(listener);
    format!("tcp://127.0.0.1:{}", port)
}

pub fn wait_connected(monitors: &[&SocketMonitor]) {
    let poller = Poller::new().expect("monitor poller creation failed");
    let mut ready = vec![false; monitors.len()];
    for (slot, monitor) in monitors.iter().enumerate() {
        poller
            .add_monitor(monitor, POLLIN, slot)
            .expect("monitor poller registration failed");
    }

    let mut events = vec![PollEvent::default(); monitors.len().max(1)];
    while ready.iter().any(|item| !item) {
        let count = poller.wait(&mut events, -1).expect("monitor poll failed");
        for poll_event in events.iter().take(count) {
            let monitor = monitors[poll_event.slot];
            while let Some(event) = monitor
                .recv_with_flags(RecvFlags::DONT_WAIT)
                .expect("monitor recv failed")
            {
                if event.is_connection_ready()
                    || monitor.status().expect("monitor status failed").is_ready()
                {
                    ready[poll_event.slot] = true;
                }
            }
        }
    }
}

pub fn wait_stream_connected(monitor: &SocketMonitor) {
    let poller = Poller::new().expect("monitor poller creation failed");
    poller
        .add_monitor(monitor, POLLIN, 0)
        .expect("monitor poller registration failed");
    let mut events = [PollEvent::default()];
    loop {
        poller.wait(&mut events, -1).expect("monitor poll failed");
        while let Some(event) = monitor
            .recv_with_flags(RecvFlags::DONT_WAIT)
            .expect("monitor recv failed")
        {
            if event.is_accepted()
                || event.is_connection_ready()
                || monitor.status().expect("monitor status failed").is_ready()
            {
                return;
            }
        }
    }
}

pub fn wait_until<F>(mut predicate: F, timeout: Duration, description: &str)
where
    F: FnMut() -> bool,
{
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        if predicate() {
            return;
        }
        std::thread::yield_now();
    }
    panic!("timed out waiting for {description}");
}
