using System.Text;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Xunit.Abstractions;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Codecs;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class EnvelopeCodecAllocationRegressionTests(ITestOutputHelper output)
{
    private const int Iterations = 128;

    [Theory]
    [InlineData(21)]
    [InlineData(22)]
    [InlineData(24)]
    [InlineData(25)]
    public void StatefulWireAvoidsGeneratedObjectGraphAllocations(int commandId)
    {
        var command =
            (Systems.Zlink.Framework.Runtime.Protocol.ServiceWireConstants.Command)commandId;
        var rid = RoutingId.From("dst");
        var actor = new ActorRef("target", 5, "mesh", rid);
        byte[] encoded = [];
        void Encode()
        {
            encoded =
                commandId < 24
                    ? Zlink.Framework.Runtime.Service.ZLinkServiceWireCodec.EncodeSpot(
                        command,
                        commandId == 22 ? 1UL : 0,
                        new(2, 3),
                        "source",
                        "target",
                        5,
                        rid,
                        6,
                        7,
                        8,
                        false
                    )
                    : Zlink.Framework.Runtime.Service.ZLinkServiceWireCodec.EncodeActor(
                        command,
                        commandId == 25 ? 1UL : 0,
                        new(2, 3),
                        actor,
                        rid,
                        6,
                        7,
                        8,
                        false
                    );
        }
        var encodeBytes = Allocated(Encode);
        var decodeBytes = Allocated(() =>
        {
            if (
                !Zlink.Framework.Runtime.Service.ZLinkServiceWireCodec.TryDecodeStateful(
                    encoded,
                    "mesh",
                    out _,
                    out _
                )
            )
                throw new InvalidOperationException("Invalid stateful fixture.");
        });
        output.WriteLine($"command={commandId}: encode={encodeBytes}, decode={decodeBytes} B/call");
        // Release final: spot decode 112 B, Actor decode 72 B; final
        // encode 616/624/432/592 B for commands 21/22/24/25. Allow 16 B
        // above the largest observed value for each matching record shape.
        var encodeLimit = commandId switch
        {
            21 => 632,
            22 => 640,
            24 => 448,
            _ => 608,
        };
        var decodeLimit = commandId < 24 ? 128 : 88;
        Assert.True(
            encodeBytes <= encodeLimit && decodeBytes <= decodeLimit,
            $"Command {commandId}: encode {encodeBytes}/{encodeLimit}, decode {decodeBytes}/{decodeLimit} B/call."
        );
    }

    // A GC suspension can retire the thread allocation context inside a call and
    // add its unused tail to the thread counter (see ProtobufNativePayloadTests).
    private long Allocated(Action action)
    {
        for (var i = 0; i < Iterations; i++)
            action();
        long allocated = 0;
        for (var measured = 0; measured < Iterations; )
        {
            var pause = GC.GetTotalPauseDuration();
            var before = GC.GetAllocatedBytesForCurrentThread();
            action();
            var bytes = GC.GetAllocatedBytesForCurrentThread() - before;
            if (GC.GetTotalPauseDuration() != pause)
                continue;
            allocated += bytes;
            measured++;
        }
        return allocated / Iterations;
    }
}
