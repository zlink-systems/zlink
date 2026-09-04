//! Request/reply Future sample -- demonstrates direct dealer/router request surfaces.

#[path = "sample_support.rs"]
mod sample_support;

use std::thread;
use std::time::Duration;

use zlink::{Context, Message, RoutingId, SocketMonitor};

fn main() {
    // --8<-- [start:doc]
    let ctx = Context::new().expect("context creation failed");
    let endpoint = sample_support::tcp_endpoint();

    let router_socket = ctx.router_socket().expect("router socket failed");
    let dealer_socket = ctx.dealer_socket().expect("dealer socket failed");
    let router_monitor = SocketMonitor::open(&router_socket).expect("router monitor open failed");
    let dealer_monitor = SocketMonitor::open(&dealer_socket).expect("dealer monitor open failed");
    let routing_id = RoutingId::from(b"request-reply-client");
    dealer_socket
        .set_routing_id(&routing_id)
        .expect("set routing id failed");
    router_socket.bind(&endpoint).expect("bind failed");
    dealer_socket.connect(&endpoint).expect("connect failed");
    sample_support::wait_connected(&[&router_monitor, &dealer_monitor]);
    drop(router_monitor);
    drop(dealer_monitor);

    // A data round trip is the transport barrier for the first REQUEST; the
    // monitor edge alone may precede request-route attachment.
    dealer_socket
        .send()
        .message(Message::try_from(b"ready").expect("barrier message failed"))
        .submit_sync()
        .expect("barrier send failed");
    let mut barrier = zlink::Received::empty();
    router_socket
        .recv(&mut barrier, zlink::RecvFlags::NONE)
        .expect("barrier recv failed");

    let expected_routing_id = routing_id;
    let router_thread = router_socket;
    let request_handler = thread::spawn(move || {
        let mut received = zlink::Received::empty();
        router_thread
            .recv(&mut received, zlink::RecvFlags::NONE)
            .expect("router recv failed");
        assert_eq!(received.parts()[0].as_str().unwrap_or("?"), "ping");
        assert_eq!(
            received
                .routing_id()
                .expect("missing routing id")
                .as_bytes(),
            expected_routing_id.as_bytes()
        );
        received
            .reply()
            .message(Message::try_from(b"pong").expect("reply message failed"))
            .submit()
            .expect("reply send failed");
        router_thread
    });

    let reply = sample_support::block_on(
        dealer_socket
            .request()
            .message(Message::try_from(b"ping").expect("request message failed"))
            .timeout(Duration::from_secs(2))
            .submit(),
    )
    .expect("dealer request submit failed");
    assert_eq!(reply[0].as_str().unwrap_or("?"), "pong");
    drop(request_handler.join().expect("request handler failed"));

    println!("[dealer-router/request-reply/future] send: \"ping\" -> recv: \"pong\"");
    // --8<-- [end:doc]
}
