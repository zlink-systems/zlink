using System.Reflection;

namespace Zlink.Framework.Runtime.Spots;

internal enum ZLinkScannedSpotHandlerKind
{
    Packet,
    Subscription,
    ActorSend,
    ActorRequest,
    Timer
}

internal sealed record ZLinkScannedSpotHandler(
    ZLinkScannedSpotHandlerKind Kind,
    Type HandlerType,
    Type SpotType,
    Type? ActorType = null,
    string? PacketName = null,
    string? ChannelName = null,
    string? Topic = null,
    string? TimerName = null,
    TimeSpan TimerPeriod = default,
    MethodInfo? Method = null,
    string? SpotNodeName = null);

internal static class ZLinkScannedSpotHandlerScanner
{
    public static IReadOnlyList<ZLinkScannedSpotHandler> Scan(Assembly assembly)
    {
        var handlers = new List<ZLinkScannedSpotHandler>();
        foreach (var type in assembly.GetTypes())
        {
            if (type.IsAbstract || type.IsInterface) continue;

            handlers.AddRange(ScanType(type));
        }

        return handlers;
    }

    private static IEnumerable<ZLinkScannedSpotHandler> ScanType(Type handlerType)
    {
        foreach (var method in handlerType.GetMethods(BindingFlags.Instance | BindingFlags.Public))
        {
            if (method.GetCustomAttribute<ZLinkSpotRequestAttribute>() is { } request)
                yield return new ZLinkScannedSpotHandler(
                    ZLinkScannedSpotHandlerKind.Packet,
                    handlerType,
                    handlerType,
                    PacketName: request.PacketName,
                    Method: method);

            if (method.GetCustomAttribute<ZLinkSpotSubscriptionAttribute>() is { } subscription)
                yield return new ZLinkScannedSpotHandler(
                    ZLinkScannedSpotHandlerKind.Subscription,
                    handlerType,
                    handlerType,
                    ChannelName: subscription.ChannelName,
                    Topic: subscription.Topic,
                    Method: method,
                    SpotNodeName: subscription.SpotNodeName);
        }

        foreach (var (definition, arguments) in ZLinkHandlerContractInspector.EnumerateGenericInterfaces(handlerType))
            if (definition == typeof(IZLinkSpotPacketHandler<,>)
                || definition == typeof(IZLinkSpotRequestHandler<,,>))
            {
                yield return new ZLinkScannedSpotHandler(
                    ZLinkScannedSpotHandlerKind.Packet,
                    handlerType,
                    arguments[0],
                    PacketName: ResolvePacketName(handlerType));
            }
            else if (definition == typeof(IZLinkSpotSubscriptionHandler<,>))
            {
                if (handlerType.GetCustomAttribute<ZLinkSpotSubscriptionHandlerAttribute>() is { } subscription)
                    yield return new ZLinkScannedSpotHandler(
                        ZLinkScannedSpotHandlerKind.Subscription,
                        handlerType,
                        arguments[0],
                        ChannelName: subscription.ChannelName,
                        Topic: subscription.Topic);
            }
            else if (definition == typeof(IZLinkSpotTimerHandler<>))
            {
                if (handlerType.GetCustomAttribute<ZLinkSpotTimerHandlerAttribute>() is { } timer)
                    yield return new ZLinkScannedSpotHandler(
                        ZLinkScannedSpotHandlerKind.Timer,
                        handlerType,
                        arguments[0],
                        TimerName: timer.Name,
                        TimerPeriod: TimeSpan.FromMilliseconds(timer.PeriodMilliseconds));
            }
            else if (definition == typeof(IZLinkEntrySpotActorSendHandler<,,>)
                     || definition == typeof(IZLinkSpotActorSendHandler<,,>))
            {
                yield return new ZLinkScannedSpotHandler(
                    ZLinkScannedSpotHandlerKind.ActorSend,
                    handlerType,
                    arguments[0],
                    arguments[1],
                    handlerType.GetCustomAttribute<ZLinkSpotActorSendHandlerAttribute>()?.PacketName);
            }
            else if (definition == typeof(IZLinkEntrySpotActorRequestHandler<,,,>)
                     || definition == typeof(IZLinkSpotActorRequestHandler<,,,>))
            {
                yield return new ZLinkScannedSpotHandler(
                    ZLinkScannedSpotHandlerKind.ActorRequest,
                    handlerType,
                    arguments[0],
                    arguments[1],
                    handlerType.GetCustomAttribute<ZLinkSpotActorRequestHandlerAttribute>()?.PacketName);
            }
    }

    private static string? ResolvePacketName(Type handlerType)
    {
        if (handlerType.GetCustomAttribute<ZLinkSpotPacketHandlerAttribute>() is { } packet) return packet.PacketName;

        if (handlerType.GetCustomAttribute<ZLinkSpotRequestHandlerAttribute>() is { } request)
            return request.PacketName;

        return null;
    }
}
