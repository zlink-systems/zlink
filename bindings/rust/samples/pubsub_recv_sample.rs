//! PUB/SUB direct recv sample – demonstrates topic publish/subscribe.

mod sample_support;

use zlink::{Context, Message, RecvFlags, SocketMonitor, TopicMessage};

fn main() {
    // --8<-- [start:doc]
    let ctx = Context::new().expect("context creation failed");
    let endpoint = sample_support::tcp_endpoint();

    let pub_sock = ctx.create_pub_socket().expect("pub socket failed");
    let sub_sock = ctx.create_sub_socket().expect("sub socket failed");

    sub_sock
        .set_subscription("prices")
        .expect("set_subscription failed");

    let pub_mon = SocketMonitor::open(&pub_sock).expect("pub monitor open failed");
    let sub_mon = SocketMonitor::open(&sub_sock).expect("sub monitor open failed");

    pub_sock.bind(&endpoint).expect("bind failed");
    sub_sock.connect(&endpoint).expect("connect failed");

    sample_support::wait_connected(&[&pub_mon, &sub_mon]);
    drop(pub_mon);
    drop(sub_mon);

    let msg = Message::try_from(b"101.25").expect("message failed");
    pub_sock
        .publish("prices")
        .message(msg)
        .submit()
        .expect("publish failed");

    let mut topic_msg = TopicMessage::empty();
    assert!(
        sub_sock
            .subscribe(&mut topic_msg, RecvFlags::NONE)
            .expect("subscribe recv failed")
    );
    let topic = topic_msg.topic().to_owned();
    assert_eq!(topic, "prices");
    let message = topic_msg.single_part_or_throw().expect("expected one part");
    let payload = message.as_str().expect("utf8 error");
    assert_eq!(payload, "101.25");
    println!("[pubsub/recv] publish: \"{topic}/101.25\" -> subscribe: \"{topic}/{payload}\"");
    // --8<-- [end:doc]
}
