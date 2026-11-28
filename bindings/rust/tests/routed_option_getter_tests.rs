use std::time::Duration;
use zlink::Context;

#[test]
fn routed_socket_options_read_native_values() {
    let ctx = Context::new().unwrap();
    let router = ctx.create_router_socket().unwrap();
    let dealer = ctx.create_dealer_socket().unwrap();
    let router = router.router_options();
    let dealer = dealer.dealer_options();
    for enabled in [true, false] {
        router.set_mandatory(enabled).unwrap();
        router.set_probe(enabled).unwrap();
        dealer.set_probe(enabled).unwrap();
        assert_eq!(router.mandatory().unwrap(), enabled);
        assert_eq!(router.probe().unwrap(), enabled);
        assert_eq!(dealer.probe().unwrap(), enabled);
    }
    let timeout = Duration::from_millis(37);
    dealer.set_request_timeout(timeout).unwrap();
    assert_eq!(dealer.request_timeout().unwrap(), timeout);
}
