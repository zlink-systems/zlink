using System.Text;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;

namespace Zlink.Framework.UnitTests;

public sealed class AdmissionQueueStreamCodecTests
{
    [Theory]
    [InlineData(ZlinkStreamMessageKind.Request, "$zlink.admission.enqueue", true)]
    [InlineData(ZlinkStreamMessageKind.Send, "$zlink.admission.position", true)]
    [InlineData(ZlinkStreamMessageKind.Send, "$zlink.admission.ticket", true)]
    [InlineData(ZlinkStreamMessageKind.Send, "$zlink.admission.enqueue", false)]
    [InlineData(ZlinkStreamMessageKind.Request, "$zlink.admission.position", false)]
    [InlineData(ZlinkStreamMessageKind.Request, "$zlink.admission.ticket", false)]
    [InlineData(ZlinkStreamMessageKind.Request, "$zlink.admission.future", false)]
    [InlineData(ZlinkStreamMessageKind.Send, "$zlink.future", false)]
    public void ReservedAdmissionNamesAreAcceptedOnlyForTheirPacketKinds(
        ZlinkStreamMessageKind kind,
        string name,
        bool accepted
    )
    {
        var codec = new ZlinkStreamHeaderCodec();
        var header = new ZlinkStreamHeader(
            kind,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.None,
            kind == ZlinkStreamMessageKind.Request ? new ZlinkStreamRequestSeq(1) : null,
            name,
            ZlinkStreamMetadata.Empty
        );
        if (accepted)
        {
            var decoded = codec.Decode(codec.Encode(header));
            Assert.Equal(kind, decoded.Kind);
            Assert.Equal(name, decoded.Name);
            return;
        }

        Assert.Equal(
            ZlinkStreamErrorCode.ValidationFailed,
            Assert.Throws<ZlinkStreamException>(() => codec.Encode(header)).Error.Code
        );

        // Keep the framing valid while supplying an otherwise reserved peer name.
        var wire = codec.Encode(header with { Name = new string('x', name.Length) }).ToArray();
        Encoding.ASCII.GetBytes(name).CopyTo(wire, wire.Length - name.Length);
        Assert.Equal(
            ZlinkStreamErrorCode.FrameDecodeFailed,
            Assert.Throws<ZlinkStreamException>(() => codec.Decode(wire)).Error.Code
        );
    }
}
