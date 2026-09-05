//! Receive Failure Contract Tests – verify EAGAIN handling and
//! direct recv error propagation.

use zlink::{Context, Received, RecvFlags, SubscriptionEvent, TopicMessage};

#[test]
fn eagain_returns_false_not_error() {
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    sock.bind("inproc://rf-eagain").unwrap();

    let mut received = Received::empty();
    let got = sock
        .recv(&mut received, RecvFlags::DONT_WAIT)
        .expect("EAGAIN must return Ok(false), not Err");
    assert!(!got, "EAGAIN must return Ok(false)");
}

#[test]
fn eagain_sub_returns_none() {
    let ctx = Context::new().unwrap();
    let sub = ctx.sub_socket().unwrap();
    sub.bind("inproc://rf-sub-eagain-target").unwrap();
    sub.set_subscription("").unwrap();

    let mut message = TopicMessage::empty();
    let result = sub.subscribe(&mut message, RecvFlags::DONT_WAIT);
    assert!(!result.unwrap(), "EAGAIN on sub must return Ok(false)");
}

#[test]
fn eagain_xpub_subscription_event_returns_none() {
    let ctx = Context::new().unwrap();
    let xpub = ctx.xpub_socket().unwrap();
    xpub.bind("inproc://rf-xpub-eagain").unwrap();

    let mut event = SubscriptionEvent::empty();
    let result = xpub.receive_subscription_event(&mut event, RecvFlags::DONT_WAIT);
    assert!(!result.unwrap(), "EAGAIN on xpub must return Ok(false)");
}

#[test]
fn direct_recv_error_not_hidden_as_empty() {
    // Verify that a non-EAGAIN recv error surfaces as Err, not Ok(None).
    // Shutdown the context → recv returns ETERM, which must be Err.
    let ctx = Context::new().unwrap();
    let sock = ctx.pair_socket().unwrap();
    sock.bind("inproc://rf-recv-error-nothidden").unwrap();

    ctx.shutdown().unwrap();

    let mut received = Received::empty();
    let result = sock.recv(&mut received, RecvFlags::DONT_WAIT);
    // ETERM is not EAGAIN → must be Err, not Ok(false)
    assert!(
        result.is_err(),
        "non-EAGAIN recv error must be Err, not hidden"
    );
}

#[test]
fn reused_receive_preserves_parts_on_no_data_and_error() {
    let ctx = Context::new().unwrap();
    let receiver = ctx.pair_socket().unwrap();
    receiver.bind("inproc://rf-reused-parts").unwrap();
    let sender = ctx.pair_socket().unwrap();
    sender.connect("inproc://rf-reused-parts").unwrap();
    let mut received = Received::empty();

    for count in [1, 2, 5, 2, 1, 4, 1] {
        let expected: Vec<Vec<u8>> = (0..count)
            .map(|part| vec![part as u8; if part + 1 == count { 0 } else { 80 }])
            .collect();
        let mut send = sender
            .send()
            .message(zlink::Message::try_from(&expected[0]).unwrap());
        for part in &expected[1..] {
            send = send.message(zlink::Message::try_from(part).unwrap());
        }
        send.submit_sync().unwrap();
        assert!(receiver.recv(&mut received, RecvFlags::NONE).unwrap());
        assert_eq!(received.parts().len(), count);
        for (actual, expected) in received.parts().iter().zip(&expected) {
            assert_eq!(actual.as_bytes(), expected);
        }
        assert!(!receiver.recv(&mut received, RecvFlags::DONT_WAIT).unwrap());
        assert_eq!(received.parts().len(), count);
        for (actual, expected) in received.parts().iter().zip(&expected) {
            assert_eq!(actual.as_bytes(), expected);
        }
    }
    ctx.shutdown().unwrap();
    assert!(receiver.recv(&mut received, RecvFlags::DONT_WAIT).is_err());
    assert_eq!(received.parts().len(), 1);
    assert!(received.parts()[0].is_empty());
}

#[test]
fn reused_subscription_replaces_topic_and_truncates_parts() {
    let ctx = Context::new().unwrap();
    let publisher = ctx.xpub_socket().unwrap();
    publisher.bind("inproc://rf-reused-topic").unwrap();
    let subscriber = ctx.sub_socket().unwrap();
    subscriber.set_subscription("topic.").unwrap();
    subscriber.connect("inproc://rf-reused-topic").unwrap();
    let mut subscription = SubscriptionEvent::empty();
    assert!(
        publisher
            .receive_subscription_event(&mut subscription, RecvFlags::NONE)
            .unwrap()
    );
    let mut received = TopicMessage::empty();
    for (round, count) in [3, 2, 1, 4, 1].into_iter().enumerate() {
        let topic = format!("topic.{round}");
        let mut publish = publisher
            .publish(&topic)
            .message(zlink::Message::try_from([round as u8; 80]).unwrap());
        for _ in 1..count {
            publish = publish.message(zlink::Message::new().unwrap());
        }
        publish.submit().unwrap();
        assert!(
            subscriber
                .subscribe(&mut received, RecvFlags::NONE)
                .unwrap()
        );
        assert_eq!(received.topic(), topic);
        assert_eq!(received.parts().len(), count);
        assert_eq!(received.parts()[0].as_bytes(), &[round as u8; 80]);
        assert!(received.parts()[1..].iter().all(zlink::Message::is_empty));
        assert!(
            !subscriber
                .subscribe(&mut received, RecvFlags::DONT_WAIT)
                .unwrap()
        );
        assert_eq!(received.topic(), topic);
        assert_eq!(received.parts().len(), count);
    }
}
