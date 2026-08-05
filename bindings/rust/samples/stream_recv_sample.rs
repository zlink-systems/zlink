//! STREAM direct recv sample – demonstrates STREAM socket with direct recv.
//! The STREAM socket binds as a server; a raw TCP client connects inward.

mod sample_support;

use std::io::{Read, Write};
use std::net::TcpStream;

use zlink::{Context, SocketMonitor};

fn main() {
    // --8<-- [start:doc]
    let ctx = Context::new().expect("context creation failed");

    let stream = ctx.stream_socket().expect("stream socket failed");
    stream.bind("tcp://127.0.0.1:0").expect("bind failed");
    let endpoint = stream.last_endpoint().expect("last_endpoint failed");
    let stream_mon = SocketMonitor::open(&stream).expect("stream monitor open failed");

    let tcp_addr = endpoint.strip_prefix("tcp://").unwrap();
    let mut tcp_client = TcpStream::connect(tcp_addr).expect("tcp connect failed");
    tcp_client.set_nodelay(true).expect("set_nodelay failed");
    sample_support::wait_stream_connected(&stream_mon);
    drop(stream_mon);

    tcp_client
        .write_all(b"hello-stream")
        .expect("tcp write failed");
    tcp_client.flush().expect("tcp flush failed");

    let mut received = zlink::Received::empty();
    stream
        .recv(&mut received, zlink::RecvFlags::NONE)
        .expect("server recv failed");
    assert_eq!(received.parts()[0].as_bytes(), b"hello-stream");
    received
        .send()
        .message(zlink::Message::try_from(b"hello-stream").expect("reply message failed"))
        .submit()
        .expect("stream reply failed");
    let mut response = [0u8; 12];
    tcp_client
        .read_exact(&mut response)
        .expect("tcp read failed");
    assert_eq!(&response, b"hello-stream");
    println!(
        "[stream/recv] send: \"hello-stream\" → recv: \"{}\"",
        received.parts()[0].as_str().unwrap()
    );
    // --8<-- [end:doc]
}
