using Microsoft.Extensions.Configuration;

namespace ShoppingMall.Server.Configuration;

public static class SampleNames
{
    public const string MeshName = "shoppingmall.workflow";
    public const string OrderProjectionTopic = "shoppingmall.order.projection";
    public const string OrderProjectionChannel = "shoppingmall.order.projection.channel";
    public const string OrderWorkflowSpotType = "shoppingmall.order-workflow";

}

public static class OrderStatuses
{
    public const string Created = "Created";
    public const string InventoryReserved = "InventoryReserved";
    public const string PaymentAuthorized = "PaymentAuthorized";
    public const string PaymentFailed = "PaymentFailed";
    public const string InventoryReleased = "InventoryReleased";
    public const string Confirmed = "Confirmed";
    public const string Failed = "Failed";
}

public static class SampleTimings
{
    public static readonly TimeSpan HttpTimeout = TimeSpan.FromSeconds(5);
    public static readonly TimeSpan WorkflowTimeout = TimeSpan.FromSeconds(8);
}

public sealed record SampleTopology(
    string RedisEndpoint,
    string RedisKeyPrefix,
    string ApiAHttpUrl,
    string ApiBHttpUrl,
    string WorkflowAHttpUrl,
    string WorkflowBHttpUrl,
    string ApiAMeshEndpoint,
    string ApiBMeshEndpoint,
    string WorkflowAMeshEndpoint,
    string WorkflowBMeshEndpoint)
{
    public static SampleRuntimeConfiguration LoadApi(string[] args) => Load(args, "api");

    public static SampleRuntimeConfiguration LoadWorkflow(string[] args) => Load(args, "workflow");

    private static SampleRuntimeConfiguration Load(string[] args, string role)
    {
        if (args.Length != 2 || args[0] != "--config")
            throw new ArgumentException("Usage: --config PATH");
        var settings = new ConfigurationBuilder()
                           .AddJsonFile(Path.GetFullPath(args[1]), optional: false, reloadOnChange: false)
                           .Build()
                           .GetRequiredSection("Sample")
                           .Get<SampleConfiguration>()
                       ?? throw new InvalidOperationException("ShoppingMall Sample configuration is empty.");
        settings.Validate(role);
        var topology = new SampleTopology(
            settings.RedisEndpoint,
            settings.RedisKeyPrefix,
            settings.ApiAHttpUrl,
            settings.ApiBHttpUrl,
            settings.WorkflowAHttpUrl,
            settings.WorkflowBHttpUrl,
            settings.ApiAMeshEndpoint,
            settings.ApiBMeshEndpoint,
            settings.WorkflowAMeshEndpoint,
            settings.WorkflowBMeshEndpoint);
        return new SampleRuntimeConfiguration(topology, settings.InstanceId, settings.LogDirectory);
    }

    public ApiInstanceTopology ForInstance(string instanceId)
    {
        return string.Equals(instanceId, "api-b", StringComparison.Ordinal)
            ? new ApiInstanceTopology(instanceId, ApiBHttpUrl, ApiBMeshEndpoint)
            : new ApiInstanceTopology("api-a", ApiAHttpUrl, ApiAMeshEndpoint);
    }

    public WorkflowInstanceTopology ForWorkflowInstance(string instanceId)
    {
        return string.Equals(instanceId, "workflow-b", StringComparison.Ordinal)
            ? new WorkflowInstanceTopology(
                instanceId,
                WorkflowBHttpUrl,
                WorkflowBMeshEndpoint)
            : new WorkflowInstanceTopology(
                "workflow-a",
                WorkflowAHttpUrl,
                WorkflowAMeshEndpoint);
    }

}

public sealed record SampleRuntimeConfiguration(
    SampleTopology Topology,
    string InstanceId,
    string LogDirectory);

public sealed class SampleConfiguration
{
    public string InstanceId { get; init; } = "";
    public string LogDirectory { get; init; } = "";
    public string RedisEndpoint { get; init; } = "";
    public string RedisKeyPrefix { get; init; } = "";
    public string ApiAHttpUrl { get; init; } = "";
    public string ApiBHttpUrl { get; init; } = "";
    public string WorkflowAHttpUrl { get; init; } = "";
    public string WorkflowBHttpUrl { get; init; } = "";
    public string ApiAMeshEndpoint { get; init; } = "";
    public string ApiBMeshEndpoint { get; init; } = "";
    public string WorkflowAMeshEndpoint { get; init; } = "";
    public string WorkflowBMeshEndpoint { get; init; } = "";

    public void Validate(string role)
    {
        Require(InstanceId, nameof(InstanceId));
        Require(LogDirectory, nameof(LogDirectory));
        Require(RedisEndpoint, nameof(RedisEndpoint));
        Require(RedisKeyPrefix, nameof(RedisKeyPrefix));
        var suffix = InstanceId.EndsWith("-b", StringComparison.Ordinal) ? "b" : "a";
        if (role == "api")
        {
            Require(suffix == "b" ? ApiBHttpUrl : ApiAHttpUrl,
                suffix == "b" ? nameof(ApiBHttpUrl) : nameof(ApiAHttpUrl));
            Require(suffix == "b" ? ApiBMeshEndpoint : ApiAMeshEndpoint,
                suffix == "b" ? nameof(ApiBMeshEndpoint) : nameof(ApiAMeshEndpoint));
            return;
        }
        if (role != "workflow")
            throw new InvalidOperationException($"Unknown ShoppingMall role '{role}'.");
        if (suffix == "b")
        {
            Require(WorkflowBHttpUrl, nameof(WorkflowBHttpUrl));
            Require(WorkflowBMeshEndpoint, nameof(WorkflowBMeshEndpoint));
        }
        else
        {
            Require(WorkflowAHttpUrl, nameof(WorkflowAHttpUrl));
            Require(WorkflowAMeshEndpoint, nameof(WorkflowAMeshEndpoint));
        }
    }

    private static void Require(string value, string name)
    {
        if (string.IsNullOrWhiteSpace(value))
            throw new InvalidOperationException($"ShoppingMall Sample.{name} is required.");
    }
}

public sealed record ApiInstanceTopology(
    string InstanceId,
    string HttpUrl,
    string MeshEndpoint);

public sealed record WorkflowInstanceTopology(
    string InstanceId,
    string HttpUrl,
    string MeshEndpoint);
