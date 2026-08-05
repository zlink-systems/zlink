using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Timers;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class SpotAutoRegistrationScannerTests
{
    [Fact]
    public void RegistrationOwnsOneFrozenHandlerCatalog()
    {
        var registration = new ZLinkFrameworkRegistration();
        registration.HandlerAssemblies.Add(typeof(SpotAutoRegistrationScannerTests).Assembly);

        registration.FreezeScannedHandlerCatalog();
        var catalog = registration.ScannedHandlerCatalog;

        Assert.Same(catalog, registration.ScannedHandlerCatalog);
        Assert.Contains(catalog.SpotHandlers, static handler =>
            handler.HandlerType == typeof(AutoRoomSubscriptionHandler));
    }

    [Fact]
    public void Scan_Reads_Spot_Handler_Metadata_From_Attributes_And_Interface_Types()
    {
        var handlers = ZLinkScannedSpotHandlerScanner
            .Scan(typeof(SpotAutoRegistrationScannerTests).Assembly)
            .Where(static handler => handler.SpotType == typeof(AutoRoomSpot)
                || handler.SpotType == typeof(AutoEntrySpot))
            .ToArray();

        Assert.Contains(handlers, static handler =>
            handler.Kind == ZLinkScannedSpotHandlerKind.Packet
            && handler.HandlerType == typeof(AutoRoomPacketHandler)
            && handler.SpotType == typeof(AutoRoomSpot)
            && handler.PacketName == "room.packet");
        Assert.Contains(handlers, static handler =>
            handler.Kind == ZLinkScannedSpotHandlerKind.Subscription
            && handler.HandlerType == typeof(AutoRoomSubscriptionHandler)
            && handler.SpotType == typeof(AutoRoomSpot)
            && handler.Topic == "room.events");
        Assert.Contains(handlers, static handler =>
            handler.Kind == ZLinkScannedSpotHandlerKind.ActorSend
            && handler.HandlerType == typeof(AutoRoomActorSendHandler)
            && handler.SpotType == typeof(AutoRoomSpot)
            && handler.ActorType == typeof(AutoActor)
            && handler.PacketName is null);
        Assert.Contains(handlers, static handler =>
            handler.Kind == ZLinkScannedSpotHandlerKind.ActorRequest
            && handler.HandlerType == typeof(AutoEntryActorRequestHandler)
            && handler.SpotType == typeof(AutoEntrySpot)
            && handler.ActorType == typeof(AutoActor)
            && handler.PacketName is null);
        Assert.Contains(handlers, static handler =>
            handler.Kind == ZLinkScannedSpotHandlerKind.Timer
            && handler.HandlerType == typeof(AutoRoomTimerHandler)
            && handler.SpotType == typeof(AutoRoomSpot)
            && handler.TimerName == "room.tick"
            && handler.TimerPeriod == TimeSpan.FromMilliseconds(250));
        Assert.Contains(handlers, static handler =>
            handler.Kind == ZLinkScannedSpotHandlerKind.Packet
            && handler.HandlerType == typeof(AutoRoomSpot)
            && handler.Method?.Name == nameof(AutoRoomSpot.RequestAsync)
            && handler.PacketName == "room.attribute.request");
        Assert.Contains(handlers, static handler =>
            handler.Kind == ZLinkScannedSpotHandlerKind.Subscription
            && handler.HandlerType == typeof(AutoRoomSpot)
            && handler.Method?.Name == nameof(AutoRoomSpot.OnEventAsync)
            && handler.SpotNodeName == "room-node"
            && handler.Topic == "room.attribute.events");
    }

    private sealed class AutoRoomSpot : IZLinkSpot<AutoActor>
    {
        public IZLinkSpotContext Context => throw new NotSupportedException();

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken) =>
            ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());

        public ValueTask OnJoinedActorAsync(
            AutoActor actor,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(
            AutoActor actor,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;

        [ZLinkSpotRequest(PacketName = "room.attribute.request")]
        public ValueTask<AutoActorReply> RequestAsync(
            AutoActorRequest request,
            CancellationToken cancellationToken) => ValueTask.FromResult(new AutoActorReply());

        [ZLinkSpotSubscription("room-node", "room-events", "room.attribute.events")]
        public ValueTask OnEventAsync(AutoRoomEvent message) => ValueTask.CompletedTask;
    }

    private sealed class AutoEntrySpot : IZLinkEntrySpot<AutoActor>
    {
        public IZLinkEntrySpotContext Context => throw new NotSupportedException();

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken)
        {
            _ = request;
            _ = cancellationToken;
            return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());
        }

        public ValueTask OnJoinedActorAsync(
            AutoActor actor,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(
            AutoActor actor,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;
    }

    private sealed class AutoActor(string actorId) : IZLinkActor
    {
        public string ActorId { get; } = actorId;

        public IZLinkActorContext Context { get; } = new TestActorContext(actorId);
    }

    private sealed record AutoRoomPacket;

    private sealed record AutoRoomEvent;

    private sealed record AutoActorMessage;

    private sealed record AutoActorRequest;

    private sealed record AutoActorReply;

    [ZLinkSpotPacketHandler("room.packet")]
    private sealed class AutoRoomPacketHandler : IZLinkSpotPacketHandler<AutoRoomSpot, AutoRoomPacket>
    {
        public ValueTask HandleAsync(
            AutoRoomSpot spot,
            AutoRoomPacket message,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;
    }

    [ZLinkSpotSubscriptionHandler("room-events", "room.events")]
    private sealed class AutoRoomSubscriptionHandler : IZLinkSpotSubscriptionHandler<AutoRoomSpot, AutoRoomEvent>
    {
        public ValueTask HandleAsync(
            AutoRoomSpot spot,
            AutoRoomEvent message,
            ZLinkPublishMessageContext context,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;
    }

    private sealed class AutoRoomActorSendHandler
        : IZLinkSpotActorSendHandler<AutoRoomSpot, AutoActor, AutoActorMessage>
    {
        public ValueTask HandleAsync(
            AutoRoomSpot spot,
            AutoActor actor,
            IZLinkMessageContext context,
            AutoActorMessage message,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;
    }

    private sealed class AutoEntryActorRequestHandler
        : IZLinkEntrySpotActorRequestHandler<AutoEntrySpot, AutoActor, AutoActorRequest, AutoActorReply>
    {
        public ValueTask<AutoActorReply> HandleAsync(
            AutoEntrySpot entrySpot,
            AutoActor actor,
            IZLinkMessageContext context,
            AutoActorRequest request,
            CancellationToken cancellationToken) => ValueTask.FromResult(new AutoActorReply());
    }

    [ZLinkSpotTimerHandler("room.tick", 250)]
    private sealed class AutoRoomTimerHandler : IZLinkSpotTimerHandler<AutoRoomSpot>
    {
        public ValueTask HandleAsync(
            AutoRoomSpot spot,
            ZLinkTimerTick tick,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;
    }
}
