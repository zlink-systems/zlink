//! STREAM packet sample – demonstrates pull-based framed receive.
//! The STREAM socket binds as a server; a raw TCP client connects inward.

#[path = "sample_support.rs"]
mod sample_support;

use std::io::Write;
use std::net::TcpStream;
use zlink::{Context, RecvFlags, SocketMonitor, StreamPacket, StreamRecvMode};

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

    let stream = ctx.stream_socket().expect("stream socket failed");
    stream
        .stream_options()
        .set_recv_mode(StreamRecvMode::Packet)
        .expect("set packet mode failed");
    let endpoint = sample_support::tcp_endpoint();
    stream.bind(&endpoint).expect("bind failed");
    let stream_mon = SocketMonitor::open(&stream).expect("stream monitor open failed");

    let tcp_addr = endpoint.strip_prefix("tcp://").unwrap();
    let mut tcp_client = TcpStream::connect(tcp_addr).expect("tcp connect failed");
    tcp_client.set_nodelay(true).expect("set_nodelay failed");
    sample_support::wait_stream_connected(&stream_mon);
    drop(stream_mon);

    write_stream_packet(&mut tcp_client, b"hello-stream");
    let mut packet = StreamPacket::empty();
    assert!(
        stream
            .recv_packet(&mut packet, RecvFlags::NONE)
            .expect("packet receive failed")
    );
    assert!(
        !packet
            .routing_id()
            .expect("packet routing id")
            .as_bytes()
            .is_empty()
    );
    assert!(
        packet
            .header()
            .expect("packet header")
            .as_bytes()
            .is_empty()
    );
    let payload = packet.body().expect("packet body").as_bytes();
    assert_eq!(payload, b"hello-stream");
    let recv_str = std::str::from_utf8(payload).unwrap();
    println!(
        "[stream/packet-recv] send: \"hello-stream\" → recv: \"{}\"",
        recv_str
    );
    packet.close().expect("packet close failed");
    // --8<-- [end:doc]
}
