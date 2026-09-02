// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Identifies a socket's messaging pattern.
/// </summary>
public enum SocketType
{
    /// <summary>
    ///     Unspecified socket type.
    /// </summary>
    Any = 0,

    /// <summary>
    ///     Exclusive one-to-one peering with no routing.
    /// </summary>
    Pair = 0x1001,

    /// <summary>
    ///     Publishes topic-filtered messages to subscribers.
    /// </summary>
    Pub = 0x1002,

    /// <summary>
    ///     Subscribes to topics published by PUB/XPUB sockets.
    /// </summary>
    Sub = 0x1003,

    /// <summary>
    ///     Load-balances messages across its connected peers.
    /// </summary>
    Dealer = 0x1004,

    /// <summary>
    ///     Routes messages to peers addressed by routing id.
    /// </summary>
    Router = 0x1005,

    /// <summary>
    ///     Publisher that also surfaces subscriber subscription events.
    /// </summary>
    XPub = 0x1006,

    /// <summary>
    ///     Subscriber whose subscriptions are carried as messages.
    /// </summary>
    XSub = 0x1007,

    /// <summary>
    ///     Exchanges framed packets with raw TCP peers.
    /// </summary>
    Stream = 0x1008
}

/// <summary>
///     Selects an automatic high-water-mark sizing profile that trades memory,
///     latency, and throughput.
/// </summary>
public enum AutoHwmProfile
{
    /// <summary>
    ///     Smallest queues, minimizing memory use.
    /// </summary>
    Compact = 0,

    /// <summary>
    ///     Small queues that drain quickly to favor latency.
    /// </summary>
    LowLatency = 1,

    /// <summary>
    ///     Balances latency against throughput.
    /// </summary>
    Balanced = 2,

    /// <summary>
    ///     Large queues that favor throughput.
    /// </summary>
    Throughput = 3
}

/// <summary>
///     The DEALER/ROUTER socket-wide receive-flow state. Control uses the
///     Application connection for count-1 peers and the Completion connection
///     for count-2 ROUTER-ROUTER peers. RUNNING and PAUSED are an absolute
///     state, not a counter: repeating the current value is a successful
///     no-op. Values match the C ABI's <c>zlink_receive_flow_state_t</c>.
///     Only DEALER and ROUTER sockets support this; other socket types throw
///     <see cref="ZlinkConfigException" /> with
///     <see cref="ZlinkConfigException.ErrorCode.NotSupported" /> and keep
///     their existing byte high-water-mark and transport backpressure
///     unchanged.
/// </summary>
public enum ReceiveFlowState
{
    /// <summary>
    ///     The socket accepts and processes inbound flow normally.
    /// </summary>
    Running = 0,

    /// <summary>
    ///     The socket has asked its paired peer to stop sending until it
    ///     resumes to <see cref="Running" />.
    /// </summary>
    Paused = 1
}

/// <summary>
///     Determines how a socket reacts to a peer that reuses an existing routing id.
/// </summary>
public enum RidDuplicatePolicy
{
    /// <summary>
    ///     Reject the new peer and keep the existing route.
    /// </summary>
    Reject = 0,

    /// <summary>
    ///     Hand the routing id to the new peer, dropping the previous holder.
    /// </summary>
    Handover = 1
}

/// <summary>
///     Determines whether a failed submit is retried.
/// </summary>
public enum SubmitRetryMode
{
    /// <summary>
    ///     Never retry; a failed submit fails immediately.
    /// </summary>
    Off = 0,

    /// <summary>
    ///     Retry when the submit fails locally, such as under back-pressure.
    /// </summary>
    LocalFailure = 1
}

/// <summary>Controls whether STREAM receives raw records or decoded packets.</summary>
public enum StreamReceiveMode
{
    /// <summary>No receive mode has been selected.</summary>
    Unspecified = 0,

    /// <summary>Receive raw STREAM records through <c>Recv</c>.</summary>
    Raw = 1,

    /// <summary>Receive decoded header/body packets through <c>RecvPacket</c>.</summary>
    Packet = 2
}

/// <summary>
///     Flags that modify send behavior.
/// </summary>
[Flags]
public enum SendFlags
{
    /// <summary>
    ///     Default behavior: block until the message can be queued.
    /// </summary>
    None = 0,

    /// <summary>
    ///     Do not block; report back-pressure instead of waiting when the send
    ///     would block.
    /// </summary>
    DontWait = 1
}

/// <summary>
///     Flags that modify receive behavior.
/// </summary>
[Flags]
public enum RecvFlags
{
    /// <summary>
    ///     Default behavior: block until a message is available.
    /// </summary>
    None = 0,

    /// <summary>
    ///     Do not block; return without a message when none is available.
    /// </summary>
    DontWait = 1
}
