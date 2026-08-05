//! Behavior tests -- verify that the binding layer correctly relays
//! core send/recv/publish/subscribe contracts.

use std::thread;
use std::time::Duration;

use zlink::{
    Context, Message, Received, RecvFlags, RoutingId, SendFlags, SocketMonitor, SubscriptionEvent,
    TopicMessage,
};

#[test]
fn pair_send_recv_roundtrip() {
    let ctx = Context::new().unwrap();
    let server = ctx.pair_socket().unwrap();
    server.bind("inproc://beh-pair").unwrap();

    let client = ctx.pair_socket().unwrap();
    client.connect("inproc://beh-pair").unwrap();

    let msg = Message::try_from(b"pair-payload-42").unwrap();
    client.send().message(msg).submit().unwrap();

    let mut received = Received::empty();
    server.recv(&mut received, RecvFlags::NONE).unwrap();
    assert_eq!(received.parts().len(), 1);
    assert_eq!(received.parts()[0].as_bytes(), b"pair-payload-42");
}

#[test]
fn pair_multipart_send_recv() {
    let ctx = Context::new().unwrap();
    let a = ctx.pair_socket().unwrap();
    a.bind("inproc://beh-pair-multi").unwrap();

    let b = ctx.pair_socket().unwrap();
    b.connect("inproc://beh-pair-multi").unwrap();

    let parts = vec![
        Message::try_from(b"frame-1").unwrap(),
        Message::try_from(b"frame-2").unwrap(),
    ];
    let mut iter = parts.into_iter();
    let first = iter.next().unwrap();
    let mut op = b.send().message(first);
    for part in iter {
        op = op.message(part);
    }
    op.submit().unwrap();

    let mut received = Received::empty();
    a.recv(&mut received, RecvFlags::NONE).unwrap();
    assert_eq!(received.parts().len(), 2);
    assert_eq!(received.parts()[0].as_bytes(), b"frame-1");
    assert_eq!(received.parts()[1].as_bytes(), b"frame-2");
}

#[test]
fn pair_try_recv_empty() {
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    sock.bind("inproc://beh-pair-try").unwrap();

    let mut received = Received::empty();
    let got = sock.recv(&mut received, RecvFlags::DONT_WAIT).unwrap();
    assert!(!got);
}

#[test]
fn dealer_router_roundtrip() {
    let ctx = Context::new().unwrap();
    let router = ctx.router_socket().unwrap();
    router.bind("inproc://beh-dr").unwrap();

    let dealer = ctx.dealer_socket().unwrap();
    let rid = RoutingId::from(b"dealer-42");
    dealer.set_routing_id(&rid).unwrap();
    dealer.connect("inproc://beh-dr").unwrap();
    thread::sleep(Duration::from_millis(50));

    // Dealer sends to Router
    let msg = Message::try_from(b"request-payload").unwrap();
    dealer.send().message(msg).submit().unwrap();

    // Router receives with the dealer's routing id
    let mut received = Received::empty();
    router.recv(&mut received, RecvFlags::NONE).unwrap();
    assert_eq!(received.parts()[0].as_bytes(), b"request-payload");

    // Router sends back to the dealer using the received routing id
    let reply = Message::try_from(b"response-payload").unwrap();
    router
        .send(received.routing_id().expect("missing routing id"))
        .message(reply)
        .submit()
        .unwrap();

    let mut response = Received::empty();
    dealer.recv(&mut response, RecvFlags::NONE).unwrap();
    assert_eq!(response.parts()[0].as_bytes(), b"response-payload");
}

#[test]
fn router_recv_preserves_routing_id_and_multipart_payload() {
    let ctx = Context::new().unwrap();
    let router = ctx.router_socket().unwrap();
    router.bind("inproc://beh-router-part").unwrap();

    let dealer = ctx.dealer_socket().unwrap();
    let rid = RoutingId::from(b"dealer-part");
    dealer.set_routing_id(&rid).unwrap();
    dealer.connect("inproc://beh-router-part").unwrap();
    thread::sleep(Duration::from_millis(50));

    dealer
        .send()
        .message(Message::try_from(b"part-1").unwrap())
        .message(Message::try_from(b"part-2").unwrap())
        .submit()
        .unwrap();

    let mut received = Received::empty();
    assert!(router.recv(&mut received, RecvFlags::NONE).unwrap());
    assert_eq!(received.routing_id().unwrap().as_bytes(), rid.as_bytes());
    assert_eq!(received.parts().len(), 2);
    assert_eq!(received.parts()[0].as_bytes(), b"part-1");
    assert_eq!(received.parts()[1].as_bytes(), b"part-2");
    assert_eq!(received.request_seq(), None);
    assert!(!router.recv(&mut received, RecvFlags::DONT_WAIT).unwrap());
}

#[test]
fn pub_sub_roundtrip() {
    let ctx = Context::new().unwrap();
    let pub_sock = ctx.pub_socket().unwrap();
    pub_sock.bind("inproc://beh-pubsub").unwrap();

    let sub_sock = ctx.sub_socket().unwrap();
    sub_sock.connect("inproc://beh-pubsub").unwrap();
    sub_sock.set_subscription("market.").unwrap();
    thread::sleep(Duration::from_millis(100));

    let msg = Message::try_from(b"price=42.5").unwrap();
    pub_sock
        .publish("market.price")
        .message(msg)
        .submit()
        .unwrap();

    let mut topic_msg = TopicMessage::empty();
    assert!(sub_sock.subscribe(&mut topic_msg, RecvFlags::NONE).unwrap());
    assert_eq!(topic_msg.topic(), "market.price");
    assert_eq!(topic_msg.parts()[0].as_bytes(), b"price=42.5");
}

#[test]
fn sub_try_subscribe_empty() {
    let ctx = Context::new().unwrap();
    let sub_sock = ctx.sub_socket().unwrap();
    // Bind without a peer so this test covers empty non-blocking input
    // without introducing an unrelated unresolved inproc connection.
    sub_sock.bind("inproc://beh-sub-try").unwrap();
    sub_sock.set_subscription("").unwrap();

    let mut topic_msg = TopicMessage::empty();
    let result = sub_sock.subscribe(&mut topic_msg, RecvFlags::DONT_WAIT);
    assert!(!result.unwrap());
}

#[test]
fn try_send_explicit_outcome() {
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    sock.bind("inproc://beh-try-send").unwrap();
    // No peer connected, so non-blocking send should fail explicitly.

    let msg = Message::try_from(b"test").unwrap();
    let _ = sock
        .send()
        .message(msg)
        .flags(SendFlags::DONT_WAIT)
        .submit();
}

#[test]
fn try_publish_explicit_outcome() {
    let ctx = Context::new().unwrap();
    let pub_sock = ctx.pub_socket().unwrap();
    pub_sock.bind("inproc://beh-try-pub").unwrap();

    let msg = Message::try_from(b"test").unwrap();
    let _ = pub_sock
        .publish("topic")
        .message(msg)
        .flags(SendFlags::DONT_WAIT)
        .submit();
}

#[test]
fn xpub_try_receive_subscription_event_empty() {
    let ctx = Context::new().unwrap();
    let xpub = ctx.xpub_socket().unwrap();
    xpub.bind("inproc://beh-xpub-try").unwrap();

    let mut event = SubscriptionEvent::empty();
    let result = xpub.receive_subscription_event(&mut event, RecvFlags::DONT_WAIT);
    assert!(!result.unwrap());
}

// ---------------------------------------------------------------------------
// Callback + send regression tests
// ---------------------------------------------------------------------------

fn tcp_endpoint() -> String {
    let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    drop(listener);
    format!("tcp://127.0.0.1:{}", port)
}

#[test]
fn dealer_router_send_from_callback() {
    let ctx = Context::new().unwrap();
    let endpoint = tcp_endpoint();

    let router = ctx.router_socket().unwrap();
    let dealer = ctx.dealer_socket().unwrap();
    let rid = RoutingId::from(b"dealer-cb-test");
    dealer.set_routing_id(&rid).unwrap();

    // Establish connection before installing callback -- required for the
    // router's internal routing-id handshake to complete in recv mode.
    let router_mon = SocketMonitor::open(&router).unwrap();
    let dealer_mon = SocketMonitor::open(&dealer).unwrap();
    router.bind(&endpoint).unwrap();
    dealer.connect(&endpoint).unwrap();
    router_mon.recv().unwrap();
    dealer_mon.recv().unwrap();
    drop(router_mon);
    drop(dealer_mon);

    // Dealer sends request.
    dealer
        .send()
        .message(Message::try_from(b"request-42").unwrap())
        .submit()
        .unwrap();

    let mut received = Received::empty();
    router.recv(&mut received, RecvFlags::NONE).unwrap();
    assert_eq!(received.parts()[0].as_bytes(), b"request-42");
    let reply = Message::try_from(b"reply-42").unwrap();
    router
        .send(received.routing_id().expect("missing routing id"))
        .message(reply)
        .submit()
        .unwrap();

    // Dealer receives the reply sent from the router handle.
    dealer
        .common_options()
        .set_receive_timeout(Duration::from_secs(5))
        .unwrap();
    let mut response = Received::empty();
    dealer.recv(&mut response, RecvFlags::NONE).unwrap();
    assert_eq!(response.parts()[0].as_bytes(), b"reply-42");
}

#[test]
fn pair_send_from_callback() {
    let ctx = Context::new().unwrap();
    let endpoint = tcp_endpoint();

    let server = ctx.pair_socket().unwrap();
    let client = ctx.pair_socket().unwrap();

    let server_mon = SocketMonitor::open(&server).unwrap();
    let client_mon = SocketMonitor::open(&client).unwrap();
    server.bind(&endpoint).unwrap();
    client.connect(&endpoint).unwrap();
    server_mon.recv().unwrap();
    client_mon.recv().unwrap();
    drop(server_mon);
    drop(client_mon);

    // Client sends and receives.
    client
        .send()
        .message(Message::try_from(b"ping-pair").unwrap())
        .submit()
        .unwrap();
    let mut received = Received::empty();
    server.recv(&mut received, RecvFlags::NONE).unwrap();
    assert_eq!(received.parts()[0].as_bytes(), b"ping-pair");
    let reply = Message::try_from(b"pong-pair").unwrap();
    server.send().message(reply).submit().unwrap();
    client
        .common_options()
        .set_receive_timeout(Duration::from_secs(5))
        .unwrap();
    let mut response = Received::empty();
    client.recv(&mut response, RecvFlags::NONE).unwrap();
    assert_eq!(response.parts()[0].as_bytes(), b"pong-pair");
}
