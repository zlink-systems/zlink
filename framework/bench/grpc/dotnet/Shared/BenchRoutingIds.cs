namespace WithGrpcBench.Shared;

/// <summary>
///     Routing ids shared by the raw ZLink bench client and the raw ZLink bench
///     server.
///     <para>
///         FB-001 / bench spec §1.3: the <c>zlink-&lt;lang&gt;</c> row is measured as
///         ROUTER↔ROUTER so that <c>zlink-framework-&lt;lang&gt; / zlink-&lt;lang&gt;</c>
///         isolates framework-layer cost instead of mixing in a DEALER→ROUTER
///         socket-pattern difference. A ROUTER addresses its peer by routing id
///         (core spec 07-router §6/§7), so the server sockets must announce a
///         well-known routing id and the client must target it.
///     </para>
/// </summary>
public static class BenchRoutingIds
{
    /// <summary>Routing id announced by the raw bench server's request ROUTER.</summary>
    public const string RawRequestServer = "bench-raw-request-server";

    /// <summary>Routing id announced by the raw bench server's command ROUTER.</summary>
    public const string RawCommandServer = "bench-raw-command-server";
}
