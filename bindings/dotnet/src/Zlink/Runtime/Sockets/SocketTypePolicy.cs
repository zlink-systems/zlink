// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed class SocketTypePolicy
{
    public SocketTypePolicy(SocketType socketType)
    {
        SocketType = socketType;
        UsesRouterRoutedReceiveEnvelope = socketType == SocketType.Router;
    }

    public SocketType SocketType { get; }
    public bool UsesRouterRoutedReceiveEnvelope { get; }

    public void EnsureSupportsMember(string memberName, SocketCapability capability)
    {
        if (Supports(capability))
            return;

        throw new InvalidOperationException(
            $"Socket type '{SocketType}' does not support '{memberName}'.");
    }

    public void EnsureOptionSupported(SocketOption option)
    {
        if (Supports(option))
            return;

        throw new InvalidOperationException(
            $"Socket option '{option}' is not supported by socket type '{SocketType}'.");
    }

    private bool Supports(SocketCapability capability)
    {
        return capability switch
        {
            SocketCapability.MessageSend => SocketType == SocketType.Pair
                                            || SocketType == SocketType.Dealer,
            SocketCapability.MessageReceive => SocketType == SocketType.Pair
                                               || SocketType == SocketType.Dealer,
            SocketCapability.RoutedSend => SocketType == SocketType.Router
                                           || SocketType == SocketType.Stream,
            SocketCapability.RoutedReceive => SocketType == SocketType.Router
                                              || SocketType == SocketType.Stream,
            SocketCapability.Publish => SocketType == SocketType.Pub
                                        || SocketType == SocketType.XPub,
            SocketCapability.SubscriptionControl => SocketType == SocketType.Sub
                                                    || SocketType == SocketType.XSub,
            SocketCapability.SubscribeReceive => SocketType == SocketType.Sub
                                                 || SocketType == SocketType.XSub,
            SocketCapability.ReceiveHandler => SocketType == SocketType.Pair
                                               || SocketType == SocketType.Dealer,
            SocketCapability.SubscriptionEvents => SocketType == SocketType.XPub,
            SocketCapability.StreamAttach => SocketType == SocketType.Stream,
            _ => false
        };
    }

    private bool Supports(SocketOption option)
    {
        return option switch
        {
            SocketOption.StreamNotify => SocketType == SocketType.Stream,
            SocketOption.XPubVerbose or SocketOption.XPubVerboser
                or SocketOption.XPubManual or SocketOption.XPubManualLastValue
                or SocketOption.XPubWelcomeMsg or SocketOption.TopicsCount
                => SocketType == SocketType.Pub || SocketType == SocketType.XPub,
            SocketOption.SubTopicsCount => SocketType == SocketType.Sub
                                           || SocketType == SocketType.XSub,
            SocketOption.XPubNoDrop => SocketType == SocketType.Pub
                                       || SocketType == SocketType.XPub,
            SocketOption.RouterMandatory => SocketType == SocketType.Router,
            SocketOption.ProbeRouter => SocketType == SocketType.Dealer
                                        || SocketType == SocketType.Router,
            SocketOption.RoutingId => SocketType == SocketType.Dealer
                                      || SocketType == SocketType.Router,
            SocketOption.ConnectRoutingId => SocketType == SocketType.Router
                                             || SocketType == SocketType.Stream,
            _ => true
        };
    }

    internal enum SocketCapability
    {
        MessageSend,
        MessageReceive,
        RoutedSend,
        RoutedReceive,
        Publish,
        SubscriptionControl,
        SubscribeReceive,
        ReceiveHandler,
        SubscriptionEvents,
        StreamAttach
    }
}
