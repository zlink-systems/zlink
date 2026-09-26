//! Projection of the Core ROUTER selected-route observation (Core ROUTER
//! §10.1): `RouterSocket::routes_snapshot`, `POLLROUTE` and the route
//! generation carried on a received ROUTER record. Scenario mirrors
//! bindings/cpp/tests/contract/test_cpp_contract_router_selected_route.cpp.

use std::net::TcpListener;
use std::time::{Duration, Instant};

use zlink::{
    Context, DealerSocket, Message, POLLROUTE, PollEvent, Poller, Received, RecvFlags,
    RidDuplicatePolicy, RouterRoute, RouterSocket, RoutingId,
};

const WAIT_MS: i64 = 5_000;

fn tcp_endpoint() -> String {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    format!("tcp://{}", listener.local_addr().unwrap())
}

fn make_router(ctx: &Context, name: &str) -> RouterSocket {
    let router = ctx.router_socket().unwrap();
    router.set_routing_id(&RoutingId::from(name)).unwrap();
    router
        .common_options()
        .set_linger(Duration::from_millis(0))
        .unwrap();
    router
        .common_options()
        .set_reconnect_interval(Duration::from_millis(0))
        .unwrap();
    router
        .common_options()
        .set_receive_timeout(Duration::from_millis(WAIT_MS as u64))
        .unwrap();
    router
        .common_options()
        .set_rid_duplicate_policy(RidDuplicatePolicy::Handover)
        .unwrap();
    router
}

fn connect_as(source: &RouterSocket, peer: &str, endpoint: &str) {
    source
        .router_options()
        .set_connect_routing_id(&RoutingId::from(peer))
        .unwrap();
    source.connect(endpoint).unwrap();
}

fn find_generation(routes: &[RouterRoute], peer: &str) -> u64 {
    let expected = RoutingId::from(peer);
    let matches: Vec<&RouterRoute> = routes
        .iter()
        .filter(|route| route.routing_id == expected)
        .collect();
    for route in routes {
        assert_ne!(route.route_generation, 0);
    }
    assert!(matches.len() <= 1);
    matches
        .first()
        .map(|route| route.route_generation)
        .unwrap_or(0)
}

/// Polls POLLROUTE on `poller` until `peer`'s selected route has a
/// generation other than `old`; returns it. Core applies route changes while
/// the socket is polled, so a socket without its own poller gets one here.
fn wait_route(socket: &RouterSocket, poller: Option<&Poller>, peer: &str, old: u64) -> u64 {
    let owned;
    let poller: &Poller = match poller {
        Some(p) => p,
        None => {
            owned = Poller::new().unwrap();
            owned.add_socket(socket, POLLROUTE, 1).unwrap();
            &owned
        }
    };
    let deadline = Instant::now() + Duration::from_millis(WAIT_MS as u64);
    let mut events = [PollEvent::default()];
    loop {
        let generation = find_generation(&socket.routes_snapshot().unwrap(), peer);
        if generation != 0 && generation != old {
            return generation;
        }
        assert!(
            Instant::now() < deadline,
            "selected route of {peer:?} did not change from {old}"
        );
        let _ = poller.wait(&mut events, 10);
    }
}

fn route_ready_now(poller: &Poller) -> bool {
    let mut events = [PollEvent::default()];
    let count = poller.wait(&mut events, 0).unwrap();
    count == 1 && events[0].revents & POLLROUTE != 0
}

fn send_text(socket: &RouterSocket, peer: &str, text: &str) {
    socket
        .send(&RoutingId::from(peer))
        .message(Message::try_from(text.as_bytes()).unwrap())
        .submit_sync()
        .unwrap();
}

fn recv_text(socket: &RouterSocket, expected: &str) -> Received {
    let mut received = Received::empty();
    assert!(socket.recv(&mut received, RecvFlags::NONE).unwrap());
    assert!(received.is_single_part());
    assert_eq!(
        received.first_part().unwrap().as_bytes(),
        expected.as_bytes()
    );
    received
}

#[test]
fn snapshot_pollroute_and_record_generation() {
    let ctx = Context::new().unwrap();
    let server = make_router(&ctx, "route-server");
    let client = make_router(&ctx, "route-client");
    let endpoint = tcp_endpoint();
    server.bind(&endpoint).unwrap();

    assert!(client.routes_snapshot().unwrap().is_empty());

    let poller = Poller::new().unwrap();
    poller.add_socket(&client, POLLROUTE, 7).unwrap();
    connect_as(&client, "route-server", &endpoint);

    let mut events = [PollEvent::default()];
    let count = poller.wait(&mut events, WAIT_MS).unwrap();
    assert_eq!(count, 1);
    assert_eq!(events[0].slot, 7);
    assert_ne!(events[0].revents & POLLROUTE, 0);

    let routes = client.routes_snapshot().unwrap();
    assert_eq!(routes.len(), 1);
    let selected = find_generation(&routes, "route-server");
    assert_ne!(selected, 0);
    // A successful snapshot that saw every change clears the level readiness.
    assert!(!route_ready_now(&poller));

    wait_route(&server, None, "route-client", 0);
    send_text(&server, "route-client", "selected");
    let received = recv_text(&client, "selected");
    assert_eq!(received.route_generation(), selected);

    // A DEALER record carries no ROUTER route generation.
    let dealer: DealerSocket = ctx.dealer_socket().unwrap();
    dealer
        .set_routing_id(&RoutingId::from("route-dealer"))
        .unwrap();
    dealer
        .common_options()
        .set_linger(Duration::from_millis(0))
        .unwrap();
    dealer
        .common_options()
        .set_receive_timeout(Duration::from_millis(WAIT_MS as u64))
        .unwrap();
    dealer.connect(&endpoint).unwrap();
    wait_route(&server, None, "route-dealer", 0);
    send_text(&server, "route-dealer", "dealer");
    let mut dealer_received = Received::empty();
    assert!(dealer.recv(&mut dealer_received, RecvFlags::NONE).unwrap());
    assert_eq!(dealer_received.route_generation(), 0);
}

#[test]
fn replaced_route_changes_generation() {
    let ctx = Context::new().unwrap();
    let old_server = make_router(&ctx, "route-server");
    let new_server = make_router(&ctx, "route-server");
    let client = make_router(&ctx, "route-client");
    let old_endpoint = tcp_endpoint();
    old_server.bind(&old_endpoint).unwrap();
    let new_endpoint = tcp_endpoint();
    new_server.bind(&new_endpoint).unwrap();

    let poller = Poller::new().unwrap();
    poller.add_socket(&client, POLLROUTE, 9).unwrap();
    connect_as(&client, "route-server", &old_endpoint);
    let first = wait_route(&client, Some(&poller), "route-server", 0);

    // Handover: the new pipe for the same RID takes over the selected route.
    connect_as(&client, "route-server", &new_endpoint);
    let second = wait_route(&client, Some(&poller), "route-server", first);
    assert_ne!(second, first);

    wait_route(&new_server, None, "route-client", 0);
    send_text(&new_server, "route-client", "new");
    let received = recv_text(&client, "new");
    assert_eq!(received.route_generation(), second);

    // The selected pipe ends: the retained standby is promoted with a new generation.
    drop(new_server);
    let promoted = wait_route(&client, Some(&poller), "route-server", second);
    assert_ne!(promoted, second);
}

#[test]
fn snapshot_grows_past_initial_capacity() {
    let ctx = Context::new().unwrap();
    let client = make_router(&ctx, "route-client");
    let mut servers = Vec::new();
    let names: Vec<String> = (0..20)
        .map(|index| format!("route-server-{index}"))
        .collect();
    for name in &names {
        let server = make_router(&ctx, name);
        let endpoint = tcp_endpoint();
        server.bind(&endpoint).unwrap();
        connect_as(&client, name, &endpoint);
        servers.push(server);
    }
    for name in &names {
        wait_route(&client, None, name, 0);
    }
    assert_eq!(client.routes_snapshot().unwrap().len(), 20);
}
