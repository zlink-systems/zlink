//! PAIR direct recv sample – demonstrates basic send/recv messaging.

mod sample_support;

use zlink::{Context, Message, SocketMonitor};

fn main() {
    // --8<-- [start:doc]
    let ctx = Context::new().expect("context creation failed");
    let endpoint = sample_support::tcp_endpoint();

    let server = ctx.pair_socket().expect("server socket failed");
    let client = ctx.pair_socket().expect("client socket failed");

    let server_mon = SocketMonitor::open(&server).expect("server monitor open failed");
    let client_mon = SocketMonitor::open(&client).expect("client monitor open failed");

    server.bind(&endpoint).expect("bind failed");
    client.connect(&endpoint).expect("connect failed");

    sample_support::wait_connected(&[&server_mon, &client_mon]);
    drop(server_mon);
    drop(client_mon);

    let msg = Message::try_from(b"hello-pair").expect("message creation failed");
    sample_support::block_on(client.send().message(msg).submit()).expect("send failed");

    let mut received = zlink::Received::empty();
    server
        .recv(&mut received, zlink::RecvFlags::NONE)
        .expect("recv failed");
    let payload = received.parts()[0].as_str().expect("utf8 error");
    assert_eq!(payload, "hello-pair");
    println!("[pair/recv] send: \"hello-pair\" → recv: \"{}\"", payload);
    // --8<-- [end:doc]
}
