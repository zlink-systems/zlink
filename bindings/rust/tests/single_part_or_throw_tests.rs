// SPDX-License-Identifier: MPL-2.0

use std::time::Duration;
use zlink::{Context, Message, Received, RecvFlags, RecvResult, SubscriptionEvent, TopicMessage};

const TIMEOUT: Option<Duration> = Some(Duration::from_secs(2));

#[test]
fn received_single_part_or_throw_transfers_only_part() {
    let empty = Received::empty().single_part_or_throw().err().unwrap();
    assert_eq!(empty.code(), RecvResult::InvalidState);

    let ctx = Context::new().unwrap();
    let receiver = ctx.create_pair_socket().unwrap();
    let sender = ctx.create_pair_socket().unwrap();
    receiver
        .common_options()
        .set_receive_timeout(TIMEOUT)
        .unwrap();
    sender.common_options().set_send_timeout(TIMEOUT).unwrap();
    receiver.bind("inproc://single-part-received").unwrap();
    sender.connect("inproc://single-part-received").unwrap();

    for count in [1, 2] {
        let mut send = sender
            .send()
            .message(Message::try_from(b"payload").unwrap());
        if count == 2 {
            send = send.message(Message::try_from(b"second").unwrap());
        }
        send.submit_sync().unwrap();

        let mut received = Received::empty();
        assert!(receiver.recv(&mut received, RecvFlags::NONE).unwrap());
        let part = received.single_part_or_throw();
        if count == 1 {
            assert_eq!(part.unwrap().as_bytes(), b"payload");
        } else {
            assert_eq!(part.err().unwrap().code(), RecvResult::InvalidState);
        }
    }
}

#[test]
fn topic_message_single_part_or_throw_transfers_only_part() {
    let empty = TopicMessage::empty().single_part_or_throw().err().unwrap();
    assert_eq!(empty.code(), RecvResult::InvalidState);

    let ctx = Context::new().unwrap();
    let publisher = ctx.create_xpub_socket().unwrap();
    let subscriber = ctx.create_sub_socket().unwrap();
    publisher
        .common_options()
        .set_receive_timeout(TIMEOUT)
        .unwrap();
    publisher
        .common_options()
        .set_send_timeout(TIMEOUT)
        .unwrap();
    subscriber
        .common_options()
        .set_receive_timeout(TIMEOUT)
        .unwrap();
    publisher.bind("inproc://single-part-topic").unwrap();
    subscriber.set_subscription("topic").unwrap();
    subscriber.connect("inproc://single-part-topic").unwrap();
    let mut event = SubscriptionEvent::empty();
    assert!(
        publisher
            .receive_subscription_event(&mut event, RecvFlags::NONE)
            .unwrap()
    );

    for count in [1, 2] {
        let mut publish = publisher
            .publish("topic")
            .message(Message::try_from(b"payload").unwrap());
        if count == 2 {
            publish = publish.message(Message::try_from(b"second").unwrap());
        }
        publish.submit().unwrap();

        let mut received = TopicMessage::empty();
        assert!(
            subscriber
                .subscribe(&mut received, RecvFlags::NONE)
                .unwrap()
        );
        let part = received.single_part_or_throw();
        if count == 1 {
            assert_eq!(part.unwrap().as_bytes(), b"payload");
        } else {
            assert_eq!(part.err().unwrap().code(), RecvResult::InvalidState);
        }
    }
}
