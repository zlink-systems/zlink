//! Monitor sample – demonstrates socket monitor event observation.

mod sample_support;

use sample_support::{tcp_endpoint, wait_connected};
use zlink::{Context, SocketMonitor};

fn main() {
    let ctx = Context::new().expect("context creation failed");
    let endpoint = tcp_endpoint();

    let server = ctx.pair_socket().expect("server socket failed");
    let server_mon = SocketMonitor::open(&server).expect("server monitor open failed");
    let client = ctx.pair_socket().expect("client socket failed");
    let client_mon = SocketMonitor::open(&client).expect("client monitor open failed");

    server.bind(&endpoint).expect("bind failed");
    client.connect(&endpoint).expect("connect failed");
    wait_connected(&[&server_mon, &client_mon]);
    println!("[monitor/recv] recv: \"connection-ready\"");
}
