use std::future::Future;
use std::pin::pin;
use std::sync::Arc;
use std::task::{Context, Poll, Wake, Waker};
use std::thread::{self, Thread};
use std::time::{Duration, Instant};

use zlink::{
    Context as ZlinkContext, DealerSocket, Message, PairSocket, Received, RecvFlags, RequestResult,
    RouterSocket, SubmitResult, ZlinkError,
};

pub(crate) type RequestReplyFuture =
    std::pin::Pin<Box<dyn Future<Output = Result<Vec<Message>, ZlinkError>> + Send>>;

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

#[allow(dead_code)]
pub(crate) fn connect_pair_and_confirm(
    bound: &PairSocket,
    connecting: &PairSocket,
    connect: impl FnOnce(),
) {
    connect();
    let submission = connecting
        .send()
        .message(Message::try_from(b"connection-ready").unwrap())
        .submit()
        .expect("connection barrier submit failed");
    block_on(submission.admitted).expect("connection barrier admission failed");
    let mut received = Received::empty();
    assert!(bound.recv(&mut received, RecvFlags::NONE).unwrap());
    assert_eq!(
        received.single_part().unwrap().as_bytes(),
        b"connection-ready"
    );
}

#[allow(dead_code)]
pub(crate) fn connect_dealer_router_and_confirm(
    router: &RouterSocket,
    dealer: &DealerSocket,
    connect: impl FnOnce(),
) {
    connect();
    let submission = dealer
        .send()
        .message(Message::try_from(b"connection-ready").unwrap())
        .submit()
        .expect("connection barrier submit failed");
    block_on(submission.admitted).expect("connection barrier admission failed");
    let mut received = Received::empty();
    assert!(router.recv(&mut received, RecvFlags::NONE).unwrap());
    assert_eq!(
        received.single_part().unwrap().as_bytes(),
        b"connection-ready"
    );
}

#[allow(dead_code)]
pub(crate) fn request_until_received(
    ctx: &ZlinkContext,
    router: &RouterSocket,
    endpoint: &str,
    payload: &[u8],
    request_timeout: Duration,
) -> (DealerSocket, SubmitResult, RequestReplyFuture, Received) {
    const READY_TIMEOUT: Duration = Duration::from_secs(5);

    let deadline = Instant::now() + READY_TIMEOUT;
    loop {
        assert!(
            Instant::now() < deadline,
            "timed out waiting for a request-capable connection"
        );
        let dealer = ctx.dealer_socket().expect("request dealer creation failed");
        dealer
            .connect(endpoint)
            .expect("request dealer connect failed");
        let submission = match dealer
            .request()
            .message(Message::try_from(payload).expect("request payload creation failed"))
            .timeout(request_timeout)
            .submit()
        {
            Ok(submission) => submission,
            Err(ZlinkError::Submit(error)) if error.code() == SubmitResult::NotConnected => {
                thread::yield_now();
                continue;
            }
            Err(error) => panic!("request submit failed: {error}"),
        };
        match block_on(submission.admitted) {
            Ok(()) => {}
            Err(error) if error.code() == SubmitResult::NotConnected => {
                thread::yield_now();
                continue;
            }
            Err(error) => panic!("request admission failed: {error}"),
        }
        let result = submission.result;
        let mut reply = submission.reply;
        let mut request = Received::empty();
        loop {
            if router
                .recv(&mut request, RecvFlags::DONT_WAIT)
                .expect("request barrier receive failed")
            {
                return (dealer, result, reply, request);
            }
            assert!(
                Instant::now() < deadline,
                "timed out waiting for the ROUTER to receive the request"
            );
            match poll_once(&mut reply) {
                Poll::Ready(Err(ZlinkError::Request(error)))
                    if error.code() == RequestResult::NotConnected =>
                {
                    thread::yield_now();
                    break;
                }
                Poll::Ready(Ok(_)) => panic!("request replied before the ROUTER received it"),
                Poll::Ready(Err(error)) => panic!("request failed before ROUTER receive: {error}"),
                Poll::Pending => thread::yield_now(),
            }
        }
    }
}

/// Polls a future once with a waker that does nothing.
///
/// SEND and REQUEST operations only make their first DONTWAIT admission attempt
/// when the returned `Future` is polled. A test that needs a live WRITABLE wait
/// token must therefore poll it at least once without awaiting the result.
#[allow(dead_code)]
pub(crate) fn poll_once<F: Future + Unpin>(future: &mut F) -> Poll<F::Output> {
    let mut context = Context::from_waker(Waker::noop());
    std::pin::Pin::new(future).poll(&mut context)
}
