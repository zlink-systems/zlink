//! PUB/SUB direct recv sample – demonstrates topic publish/subscribe.

mod sample_support;

use zlink::{Context, Message, RecvFlags, SocketMonitor, SubscriptionEvent, TopicMessage};

fn main() {
    // --8<-- [start:doc]
    let ctx = Context::new().expect("context creation failed");
    let endpoint = sample_support::tcp_endpoint();

    let pub_sock = ctx.create_xpub_socket().expect("xpub socket failed");
    let sub_sock = ctx.create_sub_socket().expect("sub socket failed");
    let topic = "prices";

    sub_sock
        .set_subscription(topic)
        .expect("set_subscription failed");

    let pub_mon = SocketMonitor::open(&pub_sock).expect("pub monitor open failed");
    let sub_mon = SocketMonitor::open(&sub_sock).expect("sub monitor open failed");

    pub_sock.bind(&endpoint).expect("bind failed");
    sub_sock.connect(&endpoint).expect("connect failed");

    sample_support::wait_connected(&[&pub_mon, &sub_mon]);
    drop(pub_mon);
    drop(sub_mon);

    pub_sock
        .common_options()
        .set_receive_timeout(1_000)
        .expect("set subscription event timeout failed");
    let mut event = SubscriptionEvent::empty();
    assert!(
        pub_sock
            .receive_subscription_event(&mut event, RecvFlags::NONE)
            .expect("subscription event receive failed"),
        "subscription event timed out"
    );
    assert!(event.is_subscribed());
    assert_eq!(event.topic(), topic);

    let msg = Message::try_from(b"101.25").expect("message failed");
    pub_sock
        .publish(topic)
        .message(msg)
        .submit()
        .expect("publish failed");

    let mut topic_msg = TopicMessage::empty();
    assert!(
        sub_sock
            .subscribe(&mut topic_msg, RecvFlags::NONE)
            .expect("subscribe recv failed")
    );
    assert_eq!(topic_msg.topic(), topic);
    assert_eq!(topic_msg.parts()[0].as_str().unwrap(), "101.25");
    println!(
        "[pubsub/recv] publish: \"{}/101.25\" → subscribe: \"{}/{}\"",
        topic_msg.topic(),
        topic_msg.topic(),
        topic_msg.parts()[0].as_str().unwrap()
    );
    // --8<-- [end:doc]
}
