package systems.zlink.samples.deliverydispatch.server.configuration;

public final class SampleNames {
    private SampleNames() {
    }

    public static final String CourierChannel = "deliverydispatch.courier";

    /**
     * Where the courier's decision comes back to. The offer goes out one-way and the decision
     * returns as its own one-way message, so dispatch has to be reachable as a channel server
     * (common sample spec section 7.4).
     */
    public static final String DispatchChannel = "deliverydispatch.dispatch";
    public static final String TrackingChannel = "deliverydispatch.tracking";
    public static final String CustomerSpotDiscovery = "delivery-customers";
    public static final String CourierSpotDiscovery = "delivery-couriers";
    public static final String CustomerStreamNode = "deliverydispatch.customer.stream";
    public static final String CourierStreamNode = "deliverydispatch.courier.stream";
    public static final String CustomerActorType = "deliverydispatch.customer.actor";
    public static final String CourierActorType = "deliverydispatch.courier.actor";

    public static final String TopologyReadyMarker = "topology=ready";
    public static final String ReassignmentMarker = "deliverydispatch-reassignment=completed";
    public static final String ServerEvidenceMarker = "deliverydispatch-server-evidence=completed";
    public static final String CompletedMarker = "deliverydispatch=completed";
}
