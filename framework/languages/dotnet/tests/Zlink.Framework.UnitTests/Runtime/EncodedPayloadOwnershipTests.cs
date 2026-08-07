using System.Runtime.InteropServices;
using Xunit.Abstractions;

namespace Zlink.Framework.UnitTests;

public sealed class EncodedPayloadOwnershipTests(ITestOutputHelper output)
{
    [Fact]
    public void PublicFactory_DefensivelyCopiesCallerBuffer()
    {
        var callerBuffer = new byte[] { 1, 2, 3 };

        var payload = ZLinkEncodedPayload.From(callerBuffer);
        callerBuffer[0] = 9;

        Assert.Equal(new byte[] { 1, 2, 3 }, payload.Bytes.ToArray());
        Assert.True(MemoryMarshal.TryGetArray(
            payload.Bytes,
            out ArraySegment<byte> stored));
        Assert.NotSame(callerBuffer, stored.Array);
    }

    [Fact]
    public void OwnedFactory_RetainsTheTransferredBufferWithoutCopying()
    {
        var ownedBuffer = new byte[] { 1, 2, 3 };

        var payload = ZLinkEncodedPayload.FromOwned(ownedBuffer);

        Assert.True(MemoryMarshal.TryGetArray(
            payload.Bytes,
            out ArraySegment<byte> stored));
        Assert.Same(ownedBuffer, stored.Array);
    }

    [Fact]
    public void OwnedFactory_DoesNotAllocateByPayloadSize()
    {
        const int iterations = 128;
        var ownedBuffer = new byte[16 * 1024];
        _ = ZLinkEncodedPayload.FromOwned(ownedBuffer);

        var before = GC.GetAllocatedBytesForCurrentThread();
        var observedBytes = 0;
        for (var index = 0; index < iterations; index++)
            observedBytes += ZLinkEncodedPayload.FromOwned(ownedBuffer).Bytes.Length;
        var ownedAllocated = GC.GetAllocatedBytesForCurrentThread() - before;

        before = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < iterations; index++)
            observedBytes += ZLinkEncodedPayload.From(ownedBuffer).Bytes.Length;
        var copiedAllocated = GC.GetAllocatedBytesForCurrentThread() - before;

        GC.KeepAlive(observedBytes);
        output.WriteLine(
            $"owned_allocated={ownedAllocated} copied_allocated={copiedAllocated} "
            + $"payload_bytes={ownedBuffer.Length} iterations={iterations}");
        Assert.True(ownedAllocated < 1_024, $"owned_allocated={ownedAllocated}");
        Assert.True(
            copiedAllocated >= ownedBuffer.Length * iterations,
            $"copied_allocated={copiedAllocated}");
    }
}
