mod test_support;

use std::time::Duration;
use zlink::{
    Context, Message, POLLCOMPLETION, POLLIN, PollEvent, Poller, ReceiveFlowState, Received,
    RecvFlags, SocketMonitor, SocketMonitorEventMask, SocketMonitorOpenOptions, SubmitResult,
};

fn wait_for_pause(monitor: &SocketMonitor) {
    let poller = Poller::new().unwrap();
    poller.add_monitor(monitor, POLLIN, 0).unwrap();
    assert_eq!(poller.wait(&mut [PollEvent::default()], 2000).unwrap(), 1);
    assert_eq!(
        monitor.recv().unwrap().event,
        zlink::MonitorEventType::SendFlowPaused
    );
}

fn pause_monitor(router: &zlink::RouterSocket) -> SocketMonitor {
    SocketMonitor::open_with_options(
        router,
        SocketMonitorOpenOptions {
            events: SocketMonitorEventMask::SEND_FLOW_PAUSED,
            monitor_hwm_bytes: 0,
        },
    )
    .unwrap()
}

#[test]
fn reply_to_paused_dealer_reports_backpressure() {
    let ctx = Context::new().unwrap();
    let router = ctx.create_router_socket().unwrap();
    let monitor = pause_monitor(&router);
    router
        .common_options()
        .set_send_timeout(Some(Duration::from_millis(10)))
        .unwrap();
    let endpoint = "inproc://rust-reply-paused-dealer";
    router.bind(endpoint).unwrap();
    let (dealer, _driver, _, _pending, request) = test_support::request_until_received(
        &ctx,
        &router,
        endpoint,
        b"request",
        Duration::from_secs(2),
    );
    dealer
        .common_options()
        .set_receive_flow_state(ReceiveFlowState::Paused)
        .unwrap();
    wait_for_pause(&monitor);
    let error = request
        .reply()
        .message(Message::try_from(b"reply").unwrap())
        .submit()
        .unwrap_err();
    assert_eq!(error.code(), SubmitResult::Backpressured);
}

#[test]
fn reply_to_paused_router_uses_the_completion_lane() {
    let ctx = Context::new().unwrap();
    let responder = ctx.create_router_socket().unwrap();
    let requester = ctx.create_router_socket().unwrap();
    let monitor = pause_monitor(&responder);
    responder
        .common_options()
        .set_send_timeout(Some(Duration::from_millis(10)))
        .unwrap();
    let endpoint = "tcp://127.0.0.1:*";
    responder.bind(endpoint).unwrap();
    requester
        .connect(&responder.last_endpoint().unwrap())
        .unwrap();
    let monitor_requester = SocketMonitor::open(&requester).unwrap();
    let readiness = Poller::new().unwrap();
    readiness
        .add_monitor(&monitor_requester, POLLIN, 0)
        .unwrap();
    let deadline = std::time::Instant::now() + Duration::from_secs(2);
    let target = loop {
        if let Some(route) = requester.routes_snapshot().unwrap().first() {
            break route.routing_id;
        }
        assert!(
            std::time::Instant::now() < deadline,
            "ROUTER route was never selected"
        );
        readiness.wait(&mut [PollEvent::default()], 25).unwrap();
        while monitor_requester
            .recv_with_flags(RecvFlags::DONT_WAIT)
            .unwrap()
            .is_some()
        {}
    };
    let owner = Poller::new().unwrap();
    owner.add_socket(&requester, POLLCOMPLETION, 0).unwrap();
    let submission = requester
        .request(&target)
        .message(Message::try_from(b"request").unwrap())
        .timeout(Duration::from_secs(2))
        .submit()
        .unwrap();
    test_support::block_on(submission.admitted).unwrap();
    responder
        .common_options()
        .set_receive_timeout(Some(Duration::from_millis(2000)))
        .unwrap();
    let mut request = Received::empty();
    assert!(responder.recv(&mut request, RecvFlags::NONE).unwrap());
    requester
        .common_options()
        .set_receive_flow_state(ReceiveFlowState::Paused)
        .unwrap();
    wait_for_pause(&monitor);
    request
        .reply()
        .message(Message::try_from(b"reply").unwrap())
        .submit()
        .unwrap();
    assert!(owner.wait(&mut [PollEvent::default()], 2000).unwrap() > 0);
    assert_eq!(
        test_support::block_on(submission.reply).unwrap()[0].as_bytes(),
        b"reply"
    );
}
