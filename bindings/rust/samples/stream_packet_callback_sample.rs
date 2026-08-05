//! STREAM callback sample – demonstrates STREAM socket with packet handler.
//! The STREAM socket binds as a server; a raw TCP client connects inward.

#[path = "sample_support.rs"]
mod sample_support;

use std::io::Write;
use std::net::TcpStream;
use std::sync::mpsc;
use std::time::Duration;

use zlink::{Context, SocketMonitor};

fn write_stream_packet(stream: &mut TcpStream, body: &[u8]) {
    let mut frame = Vec::with_capacity(6 + body.len());
    frame.extend_from_slice(&0u16.to_be_bytes());
    frame.extend_from_slice(&(body.len() as u32).to_be_bytes());
    frame.extend_from_slice(body);
    stream.write_all(&frame).expect("tcp write failed");
    stream.flush().expect("tcp flush failed");
}

fn main() {
    // --8<-- [start:doc]
    let ctx = Context::new().expect("context creation failed");

    let mut stream = ctx.stream_socket().expect("stream socket failed");
    let endpoint = sample_support::tcp_endpoint();
    stream.bind(&endpoint).expect("bind failed");
    let stream_mon = SocketMonitor::open(&stream).expect("stream monitor open failed");

    let (tx, rx) = mpsc::channel();

    stream
        .on_packet(move |routing_id, header, body| {
            assert!(!routing_id.as_bytes().is_empty());
            assert!(header.as_bytes().is_empty());
            let _ = tx.send(body.as_bytes().to_vec());
        })
        .expect("on_packet failed");

    let tcp_addr = endpoint.strip_prefix("tcp://").unwrap();
    let mut tcp_client = TcpStream::connect(tcp_addr).expect("tcp connect failed");
    tcp_client.set_nodelay(true).expect("set_nodelay failed");
    sample_support::wait_stream_connected(&stream_mon);
    drop(stream_mon);

    write_stream_packet(&mut tcp_client, b"hello-stream");
    let payload = rx
        .recv_timeout(Duration::from_secs(5))
        .expect("stream callback did not fire within 5s");
    assert_eq!(payload, b"hello-stream");
    let recv_str = std::str::from_utf8(&payload).unwrap();
    println!(
        "[stream/packet-callback] send: \"hello-stream\" → recv: \"{}\"",
        recv_str
    );
    // --8<-- [end:doc]
}
