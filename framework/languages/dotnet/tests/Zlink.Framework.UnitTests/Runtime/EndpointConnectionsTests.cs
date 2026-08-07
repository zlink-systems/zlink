using Zlink.Framework.Runtime.Configuration;

namespace Zlink.Framework.UnitTests;

public sealed class EndpointConnectionsTests
{
    [Fact]
    public void Configured_Endpoints_Are_Replayed_And_Runtime_Changes_Are_Applied()
    {
        var connections = new ZLinkEndpointConnections();
        var connected = new List<string>();
        var disconnected = new List<string>();

        connections.Connect("tcp://127.0.0.1:7101");
        connections.Connect("tcp://127.0.0.1:7101");
        connections.Attach(connected.Add, disconnected.Add);
        connections.Connect("tcp://127.0.0.1:7102");
        connections.Disconnect("tcp://127.0.0.1:7101");

        Assert.Equal(
            ["tcp://127.0.0.1:7101", "tcp://127.0.0.1:7102"],
            connected);
        Assert.Equal(["tcp://127.0.0.1:7101"], disconnected);
        Assert.Equal(["tcp://127.0.0.1:7102"], connections.ListConnections());
    }

    [Fact]
    public void New_Runtime_Generation_Replaces_Callbacks_And_Replays_Current_Endpoints()
    {
        var connections = new ZLinkEndpointConnections();
        var firstGeneration = new List<string>();
        var secondGeneration = new List<string>();
        connections.Connect("tcp://127.0.0.1:7201");

        connections.Attach(firstGeneration.Add, _ => { });
        connections.Attach(secondGeneration.Add, _ => { });
        connections.Connect("tcp://127.0.0.1:7202");

        Assert.Equal(["tcp://127.0.0.1:7201"], firstGeneration);
        Assert.Equal(
            ["tcp://127.0.0.1:7201", "tcp://127.0.0.1:7202"],
            secondGeneration);
    }

    [Fact]
    public void Disposed_Runtime_Generation_No_Longer_Receives_Endpoint_Mutations()
    {
        var connections = new ZLinkEndpointConnections();
        var firstGeneration = new List<string>();
        var secondGeneration = new List<string>();
        var firstLease = connections.Attach(firstGeneration.Add, _ => { });

        firstLease.Dispose();
        using var secondLease = connections.Attach(secondGeneration.Add, _ => { });
        connections.Connect("tcp://127.0.0.1:7301");

        Assert.Empty(firstGeneration);
        Assert.Equal(["tcp://127.0.0.1:7301"], secondGeneration);
    }

    [Fact]
    public void Older_Runtime_Lease_Cannot_Detach_The_Current_Generation()
    {
        var connections = new ZLinkEndpointConnections();
        var firstLease = connections.Attach(_ => { }, _ => { });
        var secondGeneration = new List<string>();
        using var secondLease = connections.Attach(secondGeneration.Add, _ => { });

        firstLease.Dispose();
        connections.Connect("tcp://127.0.0.1:7302");

        Assert.Equal(["tcp://127.0.0.1:7302"], secondGeneration);
    }

    [Fact]
    public void Runtime_Handle_Does_Not_Expose_Mutable_List_Operations()
    {
        IZLinkEndpointConnections connections = new ZLinkEndpointConnections();

        Assert.False(connections is IList<string>);
        Assert.True(connections is IReadOnlyCollection<string>);
    }
}
