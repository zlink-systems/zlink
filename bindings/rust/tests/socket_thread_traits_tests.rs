use zlink::{
    DealerSocket, PairSocket, PubSocket, RouterSocket, StreamSocket, SubSocket, XPubSocket,
    XSubSocket,
};

#[test]
fn all_raw_sockets_can_move_to_another_thread() {
    fn assert_send<T: Send>() {}
    assert_send::<DealerSocket>();
    assert_send::<PairSocket>();
    assert_send::<PubSocket>();
    assert_send::<RouterSocket>();
    assert_send::<StreamSocket>();
    assert_send::<SubSocket>();
    assert_send::<XPubSocket>();
    assert_send::<XSubSocket>();
}

#[test]
fn raw_sockets_do_not_offer_shared_concurrent_access() {
    trait AmbiguousIfSync<Marker> {
        fn check() {}
    }
    impl<T> AmbiguousIfSync<()> for T {}
    impl<T: Sync> AmbiguousIfSync<u8> for T {}

    // Type inference picks () only when there is no applicable Sync impl.
    let _ = <DealerSocket as AmbiguousIfSync<_>>::check;
    let _ = <PairSocket as AmbiguousIfSync<_>>::check;
    let _ = <PubSocket as AmbiguousIfSync<_>>::check;
    let _ = <RouterSocket as AmbiguousIfSync<_>>::check;
    let _ = <StreamSocket as AmbiguousIfSync<_>>::check;
    let _ = <SubSocket as AmbiguousIfSync<_>>::check;
    let _ = <XPubSocket as AmbiguousIfSync<_>>::check;
    let _ = <XSubSocket as AmbiguousIfSync<_>>::check;
}
