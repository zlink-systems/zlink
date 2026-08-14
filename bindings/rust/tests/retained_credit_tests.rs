//! Focused retained-credit receive contracts.

mod test_support;

use std::thread;
use std::time::Duration;

use zlink::{
    Context, DealerSocket, Message, PairSocket, Received, RecvError, RecvFlags, RouterSocket,
    StreamSocket, SubSocket, TopicMessage, XSubSocket,
};

fn lease_count(ctx: &Context) -> u64 {
    ctx.core_hwm_budget_snapshot()
        .expect("budget snapshot")
        .outstanding_application_lease_count
}

#[test]
fn retained_surface_is_explicit_for_every_receive_family() {
    let _: fn(&PairSocket, &mut Received, RecvFlags) -> Result<bool, RecvError> =
        PairSocket::recv_retained;
    let _: fn(&DealerSocket, &mut Received, RecvFlags) -> Result<bool, RecvError> =
        DealerSocket::recv_retained;
    let _: fn(&RouterSocket, &mut Received, RecvFlags) -> Result<bool, RecvError> =
        RouterSocket::recv_retained;
    let _: fn(&StreamSocket, &mut Received, RecvFlags) -> Result<bool, RecvError> =
        StreamSocket::recv_retained;
    let _: fn(&SubSocket, &mut TopicMessage, RecvFlags) -> Result<bool, RecvError> =
        SubSocket::subscribe_retained;
    let _: fn(&XSubSocket, &mut TopicMessage, RecvFlags) -> Result<bool, RecvError> =
        XSubSocket::subscribe_retained;
}

#[test]
fn ordinary_pair_recv_returns_credit_and_retained_releases_on_reuse_and_drop() {
    let ctx = Context::new().unwrap();
    let receiver = ctx.pair_socket().unwrap();
    receiver.bind("inproc://rust-retained-pair").unwrap();
    let sender = ctx.pair_socket().unwrap();
    sender.connect("inproc://rust-retained-pair").unwrap();
    thread::sleep(Duration::from_millis(50));

    sender
        .send()
        .message(Message::try_from(b"ordinary").unwrap())
        .submit()
        .unwrap();
    let mut ordinary = Received::empty();
    assert!(receiver.recv(&mut ordinary, RecvFlags::NONE).unwrap());
    assert_eq!(lease_count(&ctx), 0);

    sender
        .send()
        .message(Message::try_from(b"retained-1").unwrap())
        .message(Message::try_from(b"retained-2").unwrap())
        .submit()
        .unwrap();
    let mut retained = Received::empty();
    assert!(
        receiver
            .recv_retained(&mut retained, RecvFlags::NONE)
            .unwrap()
    );
    assert_eq!(retained.parts().len(), 2);
    assert_eq!(lease_count(&ctx), 2);

    assert!(
        !receiver
            .recv_retained(&mut retained, RecvFlags::DONT_WAIT)
            .unwrap()
    );
    assert_eq!(lease_count(&ctx), 0);

    sender
        .send()
        .message(Message::try_from(b"drop-release").unwrap())
        .submit()
        .unwrap();
    assert!(
        receiver
            .recv_retained(&mut retained, RecvFlags::NONE)
            .unwrap()
    );
    assert_eq!(lease_count(&ctx), 1);
    drop(retained);
    assert_eq!(lease_count(&ctx), 0);
}

#[test]
fn retained_router_and_subscribe_preserve_typed_metadata() {
    let ctx = Context::new().unwrap();
    let router = ctx.router_socket().unwrap();
    router.bind("inproc://rust-retained-router").unwrap();
    let dealer = ctx.dealer_socket().unwrap();
    let dealer_rid = zlink::RoutingId::from(b"rust-retained-dealer");
    dealer.set_routing_id(&dealer_rid).unwrap();
    dealer.connect("inproc://rust-retained-router").unwrap();
    thread::sleep(Duration::from_millis(50));

    test_support::block_on(
        dealer
            .send()
            .message(Message::try_from(b"route-1").unwrap())
            .message(Message::try_from(b"route-2").unwrap())
            .submit(),
    )
    .unwrap();
    let mut routed = Received::empty();
    assert!(router.recv_retained(&mut routed, RecvFlags::NONE).unwrap());
    assert_eq!(
        routed.routing_id().unwrap().as_bytes(),
        dealer_rid.as_bytes()
    );
    assert_eq!(routed.parts().len(), 2);
    assert_eq!(lease_count(&ctx), 2);
    drop(routed);
    assert_eq!(lease_count(&ctx), 0);

    let publisher = ctx.pub_socket().unwrap();
    publisher.bind("inproc://rust-retained-sub").unwrap();
    let subscriber = ctx.sub_socket().unwrap();
    subscriber.connect("inproc://rust-retained-sub").unwrap();
    subscriber.set_subscription("events.").unwrap();
    thread::sleep(Duration::from_millis(100));
    publisher
        .publish("events.ready")
        .message(Message::try_from(b"topic-1").unwrap())
        .message(Message::try_from(b"topic-2").unwrap())
        .submit()
        .unwrap();

    let mut topic = TopicMessage::empty();
    assert!(
        subscriber
            .subscribe_retained(&mut topic, RecvFlags::NONE)
            .unwrap()
    );
    assert_eq!(topic.topic(), "events.ready");
    assert_eq!(topic.parts().len(), 2);
    assert_eq!(lease_count(&ctx), 2);
    topic.close().unwrap();
    assert_eq!(lease_count(&ctx), 0);
}
