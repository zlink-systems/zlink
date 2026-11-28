use zlink::{
    Context, MonitorEventType, MonitorStateFlags, MonitorStatusDetailFlags, SocketMonitor,
    TransportLane,
};

#[test]
fn monitor_exposes_typed_event_and_status_fields() {
    let ctx = Context::new().unwrap();
    let router = ctx.create_router_socket().unwrap();
    let monitor = SocketMonitor::open(&router).unwrap();
    let status = monitor.status().unwrap();
    assert_eq!(
        status.state_flags.contains(MonitorStateFlags::READY),
        status.is_ready()
    );
    assert_eq!(
        status
            .detail_flags
            .contains(MonitorStatusDetailFlags::FLOW_STATE),
        status.has_flow_state_detail()
    );
    let _: fn(zlink::MonitorEvent) -> (MonitorEventType, TransportLane) =
        |event| (event.event, event.transport_lane);
    assert_ne!(
        MonitorEventType::ConnectionReady,
        MonitorEventType::Disconnected
    );
    assert_ne!(TransportLane::Application, TransportLane::Completion);
}
