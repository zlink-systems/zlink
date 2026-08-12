using System.Text.RegularExpressions;
using Xunit;

namespace Zlink.Framework.SampleRegressionTests;

public sealed partial class RegressionTests
{
    [Fact]
    public void E2E_Wire_Message_Names_Expose_Their_Call_Semantics()
    {
        var e2eRoot = ResolveE2eRoot();
        var sourceFiles = Directory
            .EnumerateFiles(e2eRoot, "*", SearchOption.AllDirectories)
            .Where(static path => (path.EndsWith(".cs", StringComparison.Ordinal)
                                   || path.EndsWith(".proto", StringComparison.Ordinal))
                                  && !path.Contains(
                                      $"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}",
                                      StringComparison.Ordinal)
                                  && !path.Contains(
                                      $"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}",
                                      StringComparison.Ordinal))
            .ToArray();
        var combined = string.Join(Environment.NewLine, sourceFiles.Select(File.ReadAllText));

        foreach (var obsoleteWireName in new[]
                 {
                     "ChannelProbeRequest",
                     "ChannelProbeReply",
                     "ChannelProbeCommand",
                     "RouteInvokeRequest",
                     "RouteInvokeResult",
                     "SendInvokeResult",
                     "ChannelObjectProbeRequest",
                     "ChannelObjectProbeReply",
                     "ChannelSpotWorkflowRequest",
                     "ChannelSpotWorkflowReply",
                     "ChannelActorJoinRequest",
                     "ChannelActorJoinReply",
                     "ChannelActorCreateRequest",
                     "ChannelActorCreateReply",
                     "ChannelSpotCreateRequest",
                     "ChannelSpotCreateReply",
                     "ChannelObjectScenarioRequest",
                     "ChannelObjectScenarioReply",
                     "ChannelBindActorRequest",
                     "ChannelBindActorReply",
                     "ChannelBoundPushRequest",
                     "ChannelBoundPushReply",
                     "ChannelBoundPushNotification",
                     "EchoAttr",
                     "EchoDi",
                     "EchoAuto",
                     "EchoManual",
                     "EchoJson",
                     "EchoJsonMsg",
                     "JsonGolden",
                     "EchoMessagePack",
                     "EchoMessagePackMsg",
                     "ActorNotify",
                     "ActorAsk",
                     "ActorReply",
                     "ActorCallRequest",
                     "ActorCallResponse",
                     "DestroyActorRequest",
                     "DestroyActorReply",
                     "BindActorRequest",
                     "BindActorReply",
                     "BoundPushRequest",
                     "BoundPushReply",
                     "AdmissionMessage",
                     "RouteReadyRequest",
                     "RouteReadyReply",
                     "SubmitResponse",
                     "FillResponse",
                     "CancellationResponse",
                     "NodeTargetOutcome",
                     "ObjectClientIdentity",
                     "OperationEvidence",
                     "HandoffPacket",
                     "RelocationWorkloadRequest",
                     "RelocationWorkloadReply",
                     "RelocationWorkloadPacket",
                     "JoinResponse",
                     "ExternalTransportGateArm",
                     "EventMsg",
                     "MissingEventMsg",
                     "ScenarioRoutePing",
                     "ScenarioRoutePong",
                     "TargetedRoutePing",
                     "WorkflowSignalReq",
                     "ObservabilityMissingPacket",
                     "InstanceColdRequestReq",
                     "InstanceColdRequestRes",
                     "InstanceColdRequest",
                     "InstanceColdRequestReply",
                     "InstanceColdSend",
                     "SpotMsg",
                     "SpotBackpressureMsg",
                     "ChannelNotify",
                     "MissingChannelNotify",
                     "LocationStoreReadProbeSnapshot",
                     "TransportGateArm"
                 })
        {
            Assert.DoesNotMatch(
                new Regex($@"\b{Regex.Escape(obsoleteWireName)}\b", RegexOptions.CultureInvariant),
                combined);
        }

        foreach (var requiredWireName in new[]
                 {
                     "ChannelProbeReq",
                     "ChannelProbeRes",
                     "ChannelProbeMsg",
                     "FanoutProbeEvent",
                     "LogicalMulticastProbeEvent",
                     "RouteRequestInvokeReq",
                     "RouteRequestInvokeRes",
                     "RouteSendInvokeReq",
                     "RouteSendInvokeRes",
                     "EchoAttrReq",
                     "EchoAttrMsg",
                     "JsonEchoReq",
                     "JsonEchoMsg",
                     "ProtobufEchoReq",
                     "ProtobufEchoRes",
                     "ProtobufEchoMsg",
                     "PackedEchoReq",
                     "PackedEchoRes",
                     "PackedEchoMsg",
                     "ActorMsg",
                     "ActorReq",
                     "ActorRes",
                     "ActorCallReq",
                     "ActorCallRes",
                     "AdmissionMsg",
                     "AdmissionEvent",
                     "RouteReadyReq",
                     "RouteReadyRes",
                     "HandoffMsg",
                     "RelocationWorkloadReq",
                     "RelocationWorkloadRes",
                     "RelocationWorkloadMsg",
                     "ActorCreateReq",
                     "CreateSpotReq",
                     "RelocationPayloadSpotReq",
                     "ScenarioActorCreateReq",
                     "EnsurePlayerReq",
                     "PublishedValueEvent",
                     "MissingTopicEvent",
                     "ScenarioRouteReq",
                     "ScenarioRouteRes",
                     "TargetedRouteReq",
                     "WorkflowSignalMsg",
                     "ObservabilityMissingReq",
                     "InstanceColdProbeReq",
                     "InstanceColdProbeRes",
                     "InstanceColdReq",
                     "InstanceColdRes",
                     "InstanceColdMsg",
                     "SpotEvent",
                     "SpotBackpressureEvent",
                     "ChannelMsg",
                     "MissingChannelMsg",
                     "ProfileEvent"
                 })
        {
            Assert.Matches(
                new Regex($@"\b{Regex.Escape(requiredWireName)}\b", RegexOptions.CultureInvariant),
                combined);
        }

        var forbiddenWireSuffix = new Regex(
            @"(?m)^\s*(?:(?:public|internal)\s+)?(?:sealed\s+)?(?:record|class|struct|message)\s+(?<name>\w+(?:Command|Result|Ack|Request|Reply|Response|Notification))\b",
            RegexOptions.CultureInvariant);
        var forbiddenSharedContracts = sourceFiles
            .Where(path => NormalizeRelativePath(Path.GetRelativePath(e2eRoot, path))
                .Split('/') is { Length: > 2 } parts
                && string.Equals(parts[1], "Shared", StringComparison.Ordinal))
            .SelectMany(path => forbiddenWireSuffix.Matches(File.ReadAllText(path))
                .Select(match => $"{NormalizeRelativePath(Path.GetRelativePath(e2eRoot, path))}:{match.Groups["name"].Value}"))
            .Order(StringComparer.Ordinal)
            .ToArray();
        Assert.True(
            forbiddenSharedContracts.Length == 0,
            "E2E shared wire contracts must use Req/Res, Msg, Notify, or publish-only Event suffixes: "
            + string.Join(", ", forbiddenSharedContracts));

        var explicitPacketName = new Regex(
            @"(?:ZLinkPacket|ZLink(?:SpotActorRequest|SpotRequest|SpotPacket)Handler|\.PacketName)\s*\(\s*\""(?<name>[A-Za-z][A-Za-z0-9]*)\""|PacketName\s*=\s*\""(?<name>[A-Za-z][A-Za-z0-9]*)\""|Add(?:RequestHandler|SendHandler|ActorPacket|Handler)<[^>]+>\s*\(\s*\""(?<name>[A-Za-z][A-Za-z0-9]*)\""",
            RegexOptions.CultureInvariant);
        var allowedWireSuffix = new Regex(
            @"(?:Req|Res|Msg|Notify|Event)$",
            RegexOptions.CultureInvariant);
        var explicitPacketNames = sourceFiles
            .SelectMany(path => explicitPacketName.Matches(File.ReadAllText(path))
                .Select(match => (
                    Path: NormalizeRelativePath(Path.GetRelativePath(e2eRoot, path)),
                    Name: match.Groups["name"].Value)))
            .ToArray();
        foreach (var requiredExplicitPacketName in new[]
                 {
                     "ChannelProbeReq",
                     "ChannelProbeMsg",
                     "FanoutProbeEvent",
                     "LogicalMulticastProbeEvent",
                     "ChannelBoundPushNotify",
                     "EchoAttrReq",
                     "EchoDiReq",
                     "EchoAutoReq",
                     "EchoManualReq",
                     "JsonEchoReq",
                     "JsonEchoMsg",
                     "JsonGoldenReq",
                     "ProtobufEchoReq",
                     "ProtobufEchoMsg",
                     "PackedEchoReq",
                     "PackedEchoRes",
                     "PackedEchoMsg",
                     "ObservabilityMissingReq"
                 })
        {
            Assert.Contains(explicitPacketNames, packet => packet.Name == requiredExplicitPacketName);
        }

        var invalidExplicitPacketNames = explicitPacketNames
            .Where(packet => !allowedWireSuffix.IsMatch(packet.Name))
            .Select(packet => $"{packet.Path}:{packet.Name}")
            .Order(StringComparer.Ordinal)
            .ToArray();
        Assert.True(
            invalidExplicitPacketNames.Length == 0,
            "Explicit E2E packet names must expose their call semantics: "
            + string.Join(", ", invalidExplicitPacketNames));

        var requestHandler = new Regex(
            @"IZLink(?:
                (?:Request|RouteRequest)Handler<\s*(?<request>\w+)\s*,\s*(?<response>\w+)\s*> |
                SpotRequestHandler<\s*\w+\s*,\s*(?<request>\w+)\s*,\s*(?<response>\w+)\s*> |
                (?:EntrySpotActor|SpotActor)RequestHandler<\s*\w+\s*,\s*\w+\s*,\s*(?<request>\w+)\s*,\s*(?<response>\w+)\s*>
            )",
            RegexOptions.CultureInvariant | RegexOptions.IgnorePatternWhitespace);
        var invalidRequestHandlers = sourceFiles
            .SelectMany(path => requestHandler.Matches(File.ReadAllText(path))
                .Where(match => !match.Groups["request"].Value.EndsWith("Req", StringComparison.Ordinal)
                                || !match.Groups["response"].Value.EndsWith("Res", StringComparison.Ordinal))
                .Select(match =>
                    $"{NormalizeRelativePath(Path.GetRelativePath(e2eRoot, path))}:"
                    + $"{match.Groups["request"].Value}/{match.Groups["response"].Value}"))
            .Order(StringComparer.Ordinal)
            .ToArray();
        Assert.True(
            invalidRequestHandlers.Length == 0,
            "E2E request handlers must use Req/Res wire types: "
            + string.Join(", ", invalidRequestHandlers));

        var sendHandler = new Regex(
            @"IZLink(?:
                (?:Send|RouteSend)Handler<\s*(?<message>\w+)\s*> |
                SpotPacketHandler<\s*\w+\s*,\s*(?<message>\w+)\s*> |
                (?:EntrySpotActor|SpotActor)SendHandler<\s*\w+\s*,\s*\w+\s*,\s*(?<message>\w+)\s*>
            )",
            RegexOptions.CultureInvariant | RegexOptions.IgnorePatternWhitespace);
        var invalidSendHandlers = sourceFiles
            .SelectMany(path => sendHandler.Matches(File.ReadAllText(path))
                .Where(match => !match.Groups["message"].Value.EndsWith("Msg", StringComparison.Ordinal))
                .Select(match =>
                    $"{NormalizeRelativePath(Path.GetRelativePath(e2eRoot, path))}:"
                    + match.Groups["message"].Value))
            .Order(StringComparer.Ordinal)
            .ToArray();
        Assert.True(
            invalidSendHandlers.Length == 0,
            "E2E one-way handlers must use Msg wire types: "
            + string.Join(", ", invalidSendHandlers));

        var publishHandler = new Regex(
            @"IZLink(?:
                FanoutHandler<\s*(?<message>\w+)\s*> |
                SpotSubscriptionHandler<\s*\w+\s*,\s*(?<message>\w+)\s*>
            )",
            RegexOptions.CultureInvariant | RegexOptions.IgnorePatternWhitespace);
        var invalidPublishHandlers = sourceFiles
            .SelectMany(path => publishHandler.Matches(File.ReadAllText(path))
                .Where(match => !match.Groups["message"].Value.EndsWith("Event", StringComparison.Ordinal))
                .Select(match =>
                    $"{NormalizeRelativePath(Path.GetRelativePath(e2eRoot, path))}:"
                    + match.Groups["message"].Value))
            .Order(StringComparer.Ordinal)
            .ToArray();
        Assert.True(
            invalidPublishHandlers.Length == 0,
            "E2E publish handlers must use Event wire types: "
            + string.Join(", ", invalidPublishHandlers));

        var directLifecyclePayload = new Regex(
            @"\.GetOrCreate\([\s\S]{0,500}?\.Request\(\s*new\s+(?<message>\w+)",
            RegexOptions.CultureInvariant);
        var invalidLifecyclePayloads = sourceFiles
            .SelectMany(path => directLifecyclePayload.Matches(File.ReadAllText(path))
                .Where(match => !match.Groups["message"].Value.EndsWith("Req", StringComparison.Ordinal))
                .Select(match =>
                    $"{NormalizeRelativePath(Path.GetRelativePath(e2eRoot, path))}:"
                    + match.Groups["message"].Value))
            .Order(StringComparer.Ordinal)
            .ToArray();
        Assert.True(
            invalidLifecyclePayloads.Length == 0,
            "E2E lifecycle create payloads must use Req wrappers: "
            + string.Join(", ", invalidLifecyclePayloads));

        Assert.DoesNotContain("IZLinkFanoutHandler<ProfileMsg>", combined, StringComparison.Ordinal);
        Assert.Contains("IZLinkFanoutHandler<ProfileEvent>", combined, StringComparison.Ordinal);
        Assert.DoesNotContain("IZLinkSpotSubscriptionHandler<MonitoringSubjectSpot, ProfileReq>", combined,
            StringComparison.Ordinal);
        Assert.Contains("IZLinkSpotSubscriptionHandler<MonitoringSubjectSpot, ProfileEvent>", combined,
            StringComparison.Ordinal);
    }
}
