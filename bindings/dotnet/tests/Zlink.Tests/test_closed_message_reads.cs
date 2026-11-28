// SPDX-License-Identifier: MPL-2.0

using System.Buffers;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_closed_message_reads
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void closed_message_reads_as_empty(bool consumed)
    {
        CheckClosedMessage(consumed, message =>
        {
            Assert.Equal(0, message.Size);
            Assert.True(message.IsEmpty);
            Assert.Equal(0, message.RefCount);
            Assert.True(message.AsSpan().IsEmpty);
            Assert.True(message.AsReadOnlySpan().IsEmpty);
            Assert.True(message.AsReadOnlyMemory().IsEmpty);
            Assert.Empty(message.ToArray());
            Assert.Empty(message.GetString());
            Assert.Equal(0, message.CopyTo(Span<byte>.Empty));
            Assert.True(message.TryCopyTo(Span<byte>.Empty, out var written));
            Assert.Equal(0, written);
            var writer = new ArrayBufferWriter<byte>();
            Assert.Equal(0, message.CopyTo(writer));
            Assert.Equal(0, writer.WrittenCount);
        });
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void copying_closed_message_creates_owned_empty_messages(bool consumed)
    {
        CheckClosedMessage(consumed, message =>
        {
            using var copy = message.Copy();
            using var clone = message.Clone();
            using var from = Message.From(message);

            Assert.True(copy.IsEmpty);
            Assert.True(clone.IsEmpty);
            Assert.True(from.IsEmpty);
            Assert.Equal(1, copy.RefCount);
            Assert.Equal(1, clone.RefCount);
            Assert.Equal(1, from.RefCount);
        });
    }

    private static void CheckClosedMessage(bool consumed, Action<Message> check)
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        using var received = Received.Create();
        using var message = Message.From("payload");
        var endpoint = CoreTestSupport.NewEndpoint("inproc", "closed-message-reads");
        router.Bind(endpoint);
        dealer.Connect(endpoint);

        if (consumed)
        {
            dealer.Send().Message(message).Submit();
            Assert.True(router.Recv(received));
            Assert.Equal("payload", received.FirstPart().GetString());
        }
        else
        {
            message.Dispose();
        }

        var error = Assert.Throws<ZlinkSubmitException>(
            () => dealer.Send().Message(message).Submit());
        Assert.Equal(ZlinkSubmitException.ErrorCode.InvalidHandle, error.Result);
        Assert.False(router.Recv(received, RecvFlags.DontWait));
        check(message);
    }
}
