// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>Core's current state and retained failure for one outbound connect intent.</summary>
/// <param name="Endpoint">The complete UTF-8 endpoint registered with Connect.</param>
/// <param name="IntentId">Nonzero opaque equality token for this intent; do not order values.</param>
/// <param name="Revision">Nonzero opaque equality token for row changes; do not order values.</param>
/// <param name="LastFailureId">Opaque equality token retained after recovery; zero before any failure. Do not order values.</param>
/// <param name="State">The state reported by Core.</param>
/// <param name="LastFailure">The most recent failure reported by Core.</param>
public readonly record struct ConnectionAttempt(string Endpoint, ulong IntentId,
    ulong Revision, ulong LastFailureId, ConnectionAttemptState State,
    ConnectionFailure LastFailure);

/// <summary>Core's normalized failure with raw diagnostic values; these do not classify request errors.</summary>
public readonly record struct ConnectionFailure(ConnectionFailureKind Kind,
    ConnectionFailureStage Stage, int NormalizedErrno, uint DisconnectReason);

/// <summary>Current connection readiness of an outbound intent.</summary>
public enum ConnectionAttemptState : uint
{
    /// <summary>No connection result has been reported yet.</summary>
    Connecting = 0,
    /// <summary>Core reports the connection ready.</summary>
    Connected = 1,
    /// <summary>No ready connection remains and the latest attempt failed.</summary>
    AttemptFailed = 2
}

/// <summary>Connection failure classification supplied by Core.</summary>
public enum ConnectionFailureKind : uint
{
    /// <summary>No failure.</summary>
    None = 0,
    /// <summary>The peer refused the connection.</summary>
    Refused = 1,
    /// <summary>The IPC endpoint is absent.</summary>
    EndpointMissing = 2,
    /// <summary>The connect or handshake deadline expired.</summary>
    TimedOut = 3,
    /// <summary>The network or host could not be reached.</summary>
    Unreachable = 4,
    /// <summary>Endpoint name resolution failed.</summary>
    NameResolution = 5,
    /// <summary>TLS authentication failed.</summary>
    AuthFailed = 6,
    /// <summary>A protocol handshake failed.</summary>
    ProtocolFailed = 7,
    /// <summary>A previously ready connection ended.</summary>
    ConnectionLost = 8,
    /// <summary>Another failure reported by Core.</summary>
    Other = 9
}

/// <summary>Connection stage of the failure supplied by Core.</summary>
public enum ConnectionFailureStage : uint
{
    /// <summary>No failure.</summary>
    None = 0,
    /// <summary>Address resolution.</summary>
    Resolve = 1,
    /// <summary>Transport establishment.</summary>
    Transport = 2,
    /// <summary>TLS handshake.</summary>
    Tls = 3,
    /// <summary>WebSocket handshake.</summary>
    WebSocket = 4,
    /// <summary>Core protocol handshake.</summary>
    CoreHandshake = 5,
    /// <summary>An established connection.</summary>
    Established = 6
}
