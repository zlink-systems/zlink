using System.Text.Json;
using Xunit;

namespace Zlink.Framework.SampleRegressionTests;

public sealed partial class RegressionTests
{
    [Fact]
    public void ZoneWorld_Uses_One_Physical_Mesh_Per_Mesh_Participant()
    {
        var sampleRoot = ResolveSampleRoot("ZoneWorld");
        var repositoryRoot = Path.GetFullPath(
            Path.Combine(ResolveDotnetRoot(), "..", "..", ".."));
        var fixturePath = Path.Combine(
            repositoryRoot,
            "framework",
            "doc",
            "framework",
            "common",
            "sample",
            "fixtures",
            "channel-topology.json");
        using var fixture = JsonDocument.Parse(File.ReadAllText(fixturePath));
        var zoneWorld = fixture.RootElement
            .GetProperty("samples")
            .GetProperty("ZoneWorld");
        Assert.Equal(
            new[] { "zoneworld.mesh" },
            zoneWorld.GetProperty("routeMeshes")
                .EnumerateArray()
                .Select(static value => value.GetString()!)
                .ToArray());
        Assert.False(
            zoneWorld.GetProperty("channels").TryGetProperty("zoneworld.actors", out _),
            "Actor routing is the Object API, not a ChannelName.");
        Assert.Equal(
            "RouteMesh",
            zoneWorld.GetProperty("channelKinds")
                .GetProperty("zoneworld.zones")
                .GetString());
        Assert.Equal(
            "RouteMesh",
            zoneWorld.GetProperty("channelKinds")
                .GetProperty("zoneworld.report")
                .GetString());

        var participants = new[]
        {
            Path.Combine(sampleRoot, "Server", "Gateway", "Program.cs"),
            Path.Combine(sampleRoot, "Server", "Ops", "Program.cs"),
            Path.Combine(sampleRoot, "Server", "ZoneNode", "Program.cs")
        };

        foreach (var participant in participants)
        {
            var source = File.ReadAllText(participant);
            Assert.Equal(1, source.Split("AddRouteMesh(", StringSplitOptions.None).Length - 1);
            Assert.Contains("AddRouteMesh(ZoneWorldNames.MeshName)", source, StringComparison.Ordinal);
            Assert.DoesNotContain("AddRequestHandler<", source, StringComparison.Ordinal);
            Assert.DoesNotContain("AddSendHandler<", source, StringComparison.Ordinal);
        }

        var zoneNode = File.ReadAllText(participants[2]);
        var gateway = File.ReadAllText(participants[0]);
        var ops = File.ReadAllText(participants[1]);
        Assert.Contains("Channel(ZoneWorldNames.ZoneChannel).Server()", zoneNode, StringComparison.Ordinal);
        Assert.DoesNotContain("Channel(ZoneWorldNames.ZoneChannel).Client()", gateway, StringComparison.Ordinal);
        Assert.DoesNotContain("Channel(ZoneWorldNames.ReportChannel).Client()", gateway, StringComparison.Ordinal);
        Assert.DoesNotContain("Channel(ZoneWorldNames.ZoneChannel).Client()", ops, StringComparison.Ordinal);
        Assert.Contains("Channel(ZoneWorldNames.ReportChannel).Client()", zoneNode, StringComparison.Ordinal);
        Assert.Contains("Channel(ZoneWorldNames.ReportChannel).Server()", ops, StringComparison.Ordinal);
        Assert.Contains("Objects().Server()", zoneNode, StringComparison.Ordinal);
        Assert.Contains("AddHandlerGroup(HandlerGroups.Ops)", ops, StringComparison.Ordinal);

        // The third ZoneNode role is the documented classic pub/sub-only
        // subscriber. It returns before RouteMesh configuration and is not a
        // second physical mesh exception.
        Assert.Contains("if (!hostsZones)", zoneNode, StringComparison.Ordinal);
        Assert.Contains("options.AddFanoutChannel", zoneNode, StringComparison.Ordinal);
    }

    [Fact]
    public void ZoneWorld_Enables_Key_Transitions_And_Persists_Process_Logs()
    {
        var sampleRoot = ResolveSampleRoot("ZoneWorld");
        var participants = new[]
        {
            Path.Combine(sampleRoot, "Server", "Gateway", "Program.cs"),
            Path.Combine(sampleRoot, "Server", "Ops", "Program.cs"),
            Path.Combine(sampleRoot, "Server", "ZoneNode", "Program.cs")
        };

        foreach (var participant in participants)
        {
            var source = File.ReadAllText(participant);
            Assert.Contains(
                ".Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal)",
                source,
                StringComparison.Ordinal);
            Assert.DoesNotContain(
                ".Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Errors)",
                source,
                StringComparison.Ordinal);
        }

        var runner = File.ReadAllText(Path.Combine(sampleRoot, "run_sample.sh"));
        Assert.Contains(
            "\"$@\" >>\"$LOG_DIR/$name.log\" 2>&1 &",
            runner,
            StringComparison.Ordinal);
    }

    [Fact]
    public void ZoneWorld_Uses_Global_Actor_Routes_After_Membership_Callbacks()
    {
        var sampleRoot = ResolveSampleRoot("ZoneWorld");
        var playerSession = File.ReadAllText(Path.Combine(
            sampleRoot,
            "Server",
            "Gateway",
            "Infrastructure",
            "ZLink",
            "Sessions",
            "PlayerSession.cs"));
        var botSpawner = File.ReadAllText(Path.Combine(
            sampleRoot,
            "Server",
            "ZoneNode",
            "Infrastructure",
            "ZLink",
            "Actors",
            "BotSpawner.cs"));
        var spot = File.ReadAllText(Path.Combine(
            sampleRoot,
            "Server",
            "ZoneNode",
            "Infrastructure",
            "ZLink",
            "Spots",
            "ZoneSpot.cs"));
        var actorHandlers = File.ReadAllText(Path.Combine(
            sampleRoot,
            "Server",
            "ZoneNode",
            "Infrastructure",
            "ZLink",
            "Spots",
            "Handlers",
            "PlayerMoveHandlers.cs"));

        Assert.DoesNotContain("Dictionary<string, PlayerActor>", spot, StringComparison.Ordinal);
        Assert.DoesNotContain("actor.Context.BoundSession", spot, StringComparison.Ordinal);
        // The sample resolves the owner per delivery through the fluent call, so the
        // send spans two lines. Pin the call itself rather than a single-line form.
        Assert.Contains(".SendToActor(playerId, message)", spot, StringComparison.Ordinal);
        Assert.Contains("PlayerZoneStateDeliveryHandler", actorHandlers, StringComparison.Ordinal);
        Assert.Contains("PlayerWorldAnnouncementDeliveryHandler", actorHandlers, StringComparison.Ordinal);
        Assert.Contains("actor.Context.BoundSession", actorHandlers, StringComparison.Ordinal);
        Assert.Contains(
            ".GetOrCreate(playerId, ZoneWorldNames.PlayerActorType)",
            playerSession,
            StringComparison.Ordinal);
        Assert.Contains(
            "spots.GetOrCreate(zoneId, ZoneWorldNames.ZoneSpotType)",
            botSpawner,
            StringComparison.Ordinal);
        Assert.Contains(
            ".GetOrCreate(route.PlayerId, ZoneWorldNames.PlayerActorType)",
            botSpawner,
            StringComparison.Ordinal);
        Assert.DoesNotContain("RequestToNode(", playerSession, StringComparison.Ordinal);
        Assert.DoesNotContain("SendToNode(", botSpawner, StringComparison.Ordinal);
    }

    [Fact]
    public void ZoneWorld_Relocation_Gate_Requires_Owner_And_Message_Follow_Evidence()
    {
        var sampleRoot = ResolveSampleRoot("ZoneWorld");
        var scenarios = File.ReadAllText(Path.Combine(sampleRoot, "Client", "Scenarios.cs"));
        var runner = File.ReadAllText(Path.Combine(sampleRoot, "run_sample.sh"));

        Assert.Contains("[\"ZW-B5\"] = B5ActorGenerationPreserved", scenarios, StringComparison.Ordinal);
        Assert.Contains("[\"ZW-B6\"] = B6MessageFollow", scenarios, StringComparison.Ordinal);
        Assert.Contains("SelectPairAsync", scenarios, StringComparison.Ordinal);
        Assert.Contains("before.ObjectGeneration == after.ObjectGeneration", scenarios, StringComparison.Ordinal);
        Assert.Contains("RequestMessageFollowProbeAsync", scenarios, StringComparison.Ordinal);
        Assert.Contains("SendMessageFollowProbeAsync", scenarios, StringComparison.Ordinal);
        Assert.Contains("message-follow-probe completed actor={playerId}", scenarios, StringComparison.Ordinal);
        Assert.Contains(
            "ZW-B2 ZW-B3 ZW-B5 ZW-B6 ZW-F2",
            runner,
            StringComparison.Ordinal);
        Assert.Contains(
            "ZW-B1 ZW-B2 ZW-B3 ZW-B4 ZW-B5 ZW-B6",
            runner,
            StringComparison.Ordinal);
        Assert.Contains("pass ZW-B6", runner, StringComparison.Ordinal);
        Assert.Contains("message_follow_relay", runner, StringComparison.Ordinal);
        Assert.Contains("payload=", runner, StringComparison.Ordinal);
        Assert.Contains("relay_hits\" -eq 2", runner, StringComparison.Ordinal);
        Assert.DoesNotContain("ZW-B6 remains withheld", runner, StringComparison.Ordinal);
    }

    [Fact]
    public void ZoneWorld_Assigns_Adjacent_Zones_To_Different_Node_Owners()
    {
        var sampleRoot = ResolveSampleRoot("ZoneWorld");
        var topology = File.ReadAllText(Path.Combine(
            sampleRoot,
            "Server",
            "Configuration",
            "ZoneTopology.cs"));
        var bootstrap = File.ReadAllText(Path.Combine(
            sampleRoot,
            "Server",
            "ZoneNode",
            "Infrastructure",
            "ZLink",
            "Actors",
            "BotSpawner.cs"));
        var fanout = File.ReadAllText(Path.Combine(
            sampleRoot,
            "Server",
            "ZoneNode",
            "Infrastructure",
            "ZLink",
            "Handlers",
            "FanoutSubscribers.cs"));
        var program = File.ReadAllText(Path.Combine(
            sampleRoot,
            "Server",
            "ZoneNode",
            "Program.cs"));

        Assert.Contains("public static IReadOnlyList<string> ZonesOf(string nodeId)", topology,
            StringComparison.Ordinal);
        Assert.Contains("NodeIds.West => [ZoneIds.NorthWest, ZoneIds.SouthWest]", topology,
            StringComparison.Ordinal);
        Assert.Contains("NodeIds.East => [ZoneIds.NorthEast, ZoneIds.SouthEast]", topology,
            StringComparison.Ordinal);
        Assert.Contains("ZoneTopology.ZonesOf(maintenance.OwnNodeId)", bootstrap,
            StringComparison.Ordinal);
        Assert.Contains("ZoneTopology.ZonesOf(maintenance.OwnNodeId)", fanout,
            StringComparison.Ordinal);
        Assert.Contains(".StableTypeLimit(2)", program, StringComparison.Ordinal);
    }

    [Fact]
    public void ZoneWorld_Same_Zone_Move_Uses_Update_Position_Message_Boundary()
    {
        var sampleRoot = ResolveSampleRoot("ZoneWorld");
        var messages = File.ReadAllText(Path.Combine(sampleRoot, "Shared", "Contracts",
            "ZoneWorldMessages.cs"));
        var movement = File.ReadAllText(Path.Combine(sampleRoot, "Server", "ZoneNode",
            "Infrastructure", "ZLink", "Spots", "Handlers", "PlayerMoveHandlers.cs"));
        var handlers = File.ReadAllText(Path.Combine(sampleRoot, "Server", "ZoneNode",
            "Infrastructure", "ZLink", "Spots", "Handlers", "ZoneSpotHandlers.cs"));
        var spot = File.ReadAllText(Path.Combine(sampleRoot, "Server", "ZoneNode",
            "Infrastructure", "ZLink", "Spots", "ZoneSpot.cs"));

        Assert.Contains("record UpdatePositionMsg", messages, StringComparison.Ordinal);
        Assert.Contains("IZLinkSpotClient spots", movement, StringComparison.Ordinal);
        Assert.Contains("SendToSpot(", movement, StringComparison.Ordinal);
        Assert.Contains("new UpdatePositionMsg", movement, StringComparison.Ordinal);
        Assert.DoesNotContain("spot.UpdatePosition(", movement, StringComparison.Ordinal);
        Assert.Contains("ZLinkSpotPacketHandler(nameof(UpdatePositionMsg))", handlers,
            StringComparison.Ordinal);
        Assert.Contains("IZLinkSpotPacketHandler<ZoneSpot, UpdatePositionMsg>", handlers,
            StringComparison.Ordinal);
        Assert.Contains("ApplyPositionUpdate", spot, StringComparison.Ordinal);
    }
}
