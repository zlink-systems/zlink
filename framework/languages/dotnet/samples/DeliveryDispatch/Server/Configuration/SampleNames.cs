namespace DeliveryDispatch.Server.Configuration;

public static class SampleNames
{
    public const string CourierMeshName = "deliverydispatch.courier";
    public const string CustomerMeshName = "deliverydispatch.customer";
    public const string DispatchChannel = "deliverydispatch.dispatch";
    public const string TrackingRouteChannel = "deliverydispatch.tracking";
    public const string CustomerSpotNode = "delivery-customer-node";
    public const string CourierSessionSpotNode = "delivery-courier-session-node";
    public const string CourierActorNode1 = "delivery-courier-node-1";
    public const string CourierActorNode2 = "delivery-courier-node-2";
    public const string CourierEntrySpotNode1 = "delivery-courier-entry-1";
    public const string CourierEntrySpotNode2 = "delivery-courier-entry-2";
    public const string CustomerStreamNode = "delivery-customer-stream";
    public const string CourierStreamNode = "delivery-courier-stream";
    public const string CustomerActorType = "delivery-customer";
    public const string CourierActorType = "delivery-courier";
}

public static class SampleTimings
{
    public static readonly TimeSpan FrameworkTimeout = TimeSpan.FromSeconds(5);
    /// <summary>How long an offer stands before the sweeper reassigns it. The deadline belongs
    /// to the dispatch worker, not to the courier node (common sample spec §7.4).</summary>
    public static readonly TimeSpan CourierDecisionTimeout = TimeSpan.FromMilliseconds(700);

    public static readonly TimeSpan OfferSweepInterval = TimeSpan.FromMilliseconds(100);
    public static readonly TimeSpan ClientTimeout = TimeSpan.FromSeconds(12);
}
