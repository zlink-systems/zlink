//! DEALER/ROUTER direct recv sample – demonstrates routed messaging.

mod sample_support;

use zlink::{Context, Message, RoutingId, SocketMonitor};

fn main() {
    // --8<-- [start:doc]
    let ctx = Context::new().expect("context creation failed");
    let endpoint = sample_support::tcp_endpoint();

    let router = ctx.router_socket().expect("router socket failed");
    let dealer = ctx.dealer_socket().expect("dealer socket failed");
    let rid = RoutingId::from(b"dealer-node-7");
    dealer.set_routing_id(&rid).expect("set routing id failed");

    let router_mon = SocketMonitor::open(&router).expect("router monitor open failed");
    let dealer_mon = SocketMonitor::open(&dealer).expect("dealer monitor open failed");

    router.bind(&endpoint).expect("bind failed");
    dealer.connect(&endpoint).expect("connect failed");

    sample_support::wait_connected(&[&router_mon, &dealer_mon]);
    drop(router_mon);
    drop(dealer_mon);

    let req = Message::try_from(b"ping").expect("message failed");
    sample_support::block_on(dealer.send().message(req).submit()).expect("send failed");

    let mut received = zlink::Received::empty();
    router
        .recv(&mut received, zlink::RecvFlags::NONE)
        .expect("router recv failed");
    assert!(received.routing_id().is_some());
    assert_eq!(received.parts()[0].as_str().unwrap(), "ping");

    let resp = Message::try_from(b"pong").expect("message failed");
    received
        .send()
        .message(resp)
        .submit()
        .expect("received send failed");

    let mut response = zlink::Received::empty();
    dealer
        .recv(&mut response, zlink::RecvFlags::NONE)
        .expect("dealer recv failed");
    assert_eq!(response.parts()[0].as_str().unwrap(), "pong");
    println!(
        "[dealer-router/recv] send: \"ping\" → recv: \"{}\"",
        response.parts()[0].as_str().unwrap()
    );
    // --8<-- [end:doc]
}
