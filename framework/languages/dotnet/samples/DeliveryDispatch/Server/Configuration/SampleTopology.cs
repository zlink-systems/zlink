namespace DeliveryDispatch.Server.Configuration;

/// <summary>
/// The endpoints as they appear in the configuration file. The runner decides the ports for a run,
/// but it hands them over in a file rather than through the environment
/// (framework/doc/framework/common/sample-e2e-configuration-policy.ko.md §2.2).
/// </summary>
public sealed class SampleTopologyOptions
{
    public string RedisEndpoint { get; set; } = string.Empty;

    public string RedisKeyPrefix { get; set; } = string.Empty;

    public string DispatchHttpUrl { get; set; } = string.Empty;

    public string MeshEndpoint { get; set; } = string.Empty;

    public string CustomerStreamEndpoint { get; set; } = string.Empty;

    public string CourierStreamEndpoint { get; set; } = string.Empty;

    /// <summary>Turns the file's values into the typed topology, failing on the first one that is
    /// missing. MeshNode routing ids are allocated by the framework at runtime.</summary>
    public SampleTopology ToTopology(string role)
    {
        var dispatch = role == "dispatch";
        var tracking = role == "tracking";
        var customer = role == "customer-gateway";
        var courierSession = role == "courier-session";
        var courierNode1 = role == "courier-actor-node1";
        var courierNode2 = role == "courier-actor-node2";
        if (!dispatch && !tracking && !customer && !courierSession && !courierNode1 && !courierNode2)
            throw new InvalidOperationException($"Unknown DeliveryDispatch server role '{role}'.");
        return new SampleTopology(
            Required(RedisEndpoint, nameof(RedisEndpoint)),
            Required(RedisKeyPrefix, nameof(RedisKeyPrefix)),
            Select(dispatch, DispatchHttpUrl, nameof(DispatchHttpUrl)),
            Required(MeshEndpoint, nameof(MeshEndpoint)),
            Select(customer, CustomerStreamEndpoint, nameof(CustomerStreamEndpoint)),
            Select(courierSession, CourierStreamEndpoint, nameof(CourierStreamEndpoint)),
            courierNode1 || courierNode2);
    }

    private static string Select(bool required, string value, string name) =>
        required ? Required(value, name) : string.Empty;

    private static string Required(string value, string name)
    {
        if (string.IsNullOrWhiteSpace(value))
            throw new InvalidOperationException($"sample.topology.{name} is required.");

        return value;
    }
}

public sealed record SampleTopology(
    string RedisEndpoint,
    string RedisKeyPrefix,
    string DispatchHttpUrl,
    string MeshEndpoint,
    string CustomerStreamEndpoint,
    string CourierStreamEndpoint,
    bool CourierActorNode);
