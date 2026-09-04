using System.Reflection;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_socket_surface
{
    private static MethodInfo[] PublicMethods(Type type) =>
        type.GetMethods(BindingFlags.Instance | BindingFlags.Public);

    [Fact]
    public void send_and_request_expose_contract_b_terminals()
    {
        Assert.Equal(typeof(SendOperation),
            typeof(IDealerSocket).GetMethod(nameof(IDealerSocket.Send))!.ReturnType);
        Assert.Equal(typeof(SendOperation),
            typeof(IRouterSocket).GetMethod(nameof(IRouterSocket.Send),
                [typeof(RoutingId)])!.ReturnType);
        Assert.Equal(typeof(SendOperation),
            typeof(IStreamSocket).GetMethod(nameof(IStreamSocket.Send),
                [typeof(RoutingId)])!.ReturnType);

        MethodInfo[] sendTerminals = PublicMethods(typeof(SendSubmitOperation));
        Assert.Contains(sendTerminals, method => method.Name == "Submit"
            && method.ReturnType == typeof(void)
            && method.GetParameters().Length == 0);
        Assert.Contains(sendTerminals, method => method.Name == "TrySubmit"
            && method.ReturnType == typeof(bool)
            && method.GetParameters().Length == 0);
        Assert.Contains(sendTerminals, method => method.Name == "Async"
            && method.ReturnType == typeof(Task)
            && method.GetParameters().Select(p => p.ParameterType)
                .SequenceEqual([typeof(CancellationToken)]));
        Assert.DoesNotContain(sendTerminals, method => method.Name == "Flags");

        MethodInfo[] requestTerminals = PublicMethods(
            typeof(RequestSubmitOperation));
        Assert.Contains(requestTerminals, method => method.Name == "Submit"
            && method.ReturnType == typeof(IReadOnlyList<Message>)
            && method.GetParameters().Length == 0);
        Assert.Contains(requestTerminals, method => method.Name == "Async"
            && method.ReturnType == typeof(Task<IReadOnlyList<Message>>));
        Assert.DoesNotContain(requestTerminals, method =>
            method.Name is "Flags" or "Callback");
    }

    [Fact]
    public void completion_kind_exposes_contract_b_values()
    {
        Assert.Equal(1, (int)CompletionKind.Send);
        Assert.Equal(2, (int)CompletionKind.Request);
        Assert.Equal(3, (int)CompletionKind.Writable);
    }

    [Fact]
    public void removed_push_and_raw_capabilities_are_absent()
    {
        Type[] exported = typeof(Zlink).Assembly.GetExportedTypes();
        string[] removedTypes =
        {
            "Routed" + "SendOperation", "Routed" + "SendSubmitOperation",
            "Request" + "Callback", "StreamPacketHandler"
        };
        foreach (string name in removedTypes)
            Assert.DoesNotContain(exported, type => type.Name == name);

        Assert.Null(typeof(IStreamSocket).GetMethod("Try" + "Send"));
        Assert.Null(typeof(IStreamSocket).GetMethod("RecvPart"));
        Assert.Null(typeof(IStreamSocket).GetMethod("OnPacket"));
        Assert.Null(typeof(ISocketMonitor).GetMethod("OnEvent"));
        Assert.Null(typeof(IZlinkTimer).GetMethod("OnFire"));
        Assert.Null(typeof(Received).GetProperty("Request" + "Seq"));
        Assert.Null(typeof(Received).GetProperty("TransportPairId"));
        Assert.Null(typeof(Received).GetProperty("TransportPairGeneration"));
    }

    [Fact]
    public void reply_token_and_stream_packet_have_only_opaque_pull_surface()
    {
        Assert.Empty(typeof(ReplyToken).GetConstructors(
            BindingFlags.Public | BindingFlags.Instance));
        Assert.Null(typeof(ReplyToken).GetMethod("op_Implicit",
            BindingFlags.Public | BindingFlags.Static));
        Assert.Null(typeof(ReplyToken).GetMethod("op_Explicit",
            BindingFlags.Public | BindingFlags.Static));
        Assert.Equal(typeof(ReplyToken),
            typeof(Received).GetProperty(nameof(Received.ReplyToken))!
                .PropertyType);

        Assert.NotNull(typeof(StreamPacket).GetMethod(nameof(StreamPacket.Create),
            BindingFlags.Public | BindingFlags.Static));
        Assert.NotNull(typeof(IStreamSocket).GetMethod(
            nameof(IStreamSocket.RecvPacket),
            [typeof(StreamPacket), typeof(RecvFlags)]));
    }
}
