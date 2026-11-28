use zlink::{ConfigResult, Context};

#[test]
fn peer_weight_checks_the_public_bounds_before_native_handle_state() {
    let ctx = Context::new().unwrap();
    let mut router = ctx.create_router_socket().unwrap();
    let mut dealer = ctx.create_dealer_socket().unwrap();
    for valid in [0, 10_000] {
        router.router_options().set_weight(valid).unwrap();
        dealer.dealer_options().set_weight(valid).unwrap();
        assert_eq!(router.router_options().weight().unwrap(), valid);
        assert_eq!(dealer.dealer_options().weight().unwrap(), valid);
    }
    router.close().unwrap();
    dealer.close().unwrap();
    assert_eq!(
        router.router_options().set_weight(10_001).unwrap_err().code,
        ConfigResult::InvalidArgument
    );
    assert_eq!(
        dealer.dealer_options().set_weight(10_001).unwrap_err().code,
        ConfigResult::InvalidArgument
    );
}
