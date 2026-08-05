using System;
using System.Reflection;
using System.Text;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_message
{
    [Fact]
    public void message_move_is_not_public_contract()
    {
        Assert.Null(typeof(Message).GetMethod("Move",
            BindingFlags.Public | BindingFlags.Instance,
            binder: null,
            Type.EmptyTypes,
            modifiers: null));
    }

    [Fact]
    public void native_message_copy_increments_ref_count_for_shared_storage()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        byte[] payload = new byte[512];
        new Random(1234).NextBytes(payload);
        using var source = new Message((ReadOnlySpan<byte>)payload);
        using Message copy = source.Copy();

        Assert.True(source.RefCount >= 2);
        Assert.True(copy.RefCount >= 2);
        Assert.True(copy.AsReadOnlySpan().SequenceEqual(source.AsReadOnlySpan()));
    }

    [Fact]
    public void message_from_copy_preserves_snapshot_storage()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        byte[] payload = new byte[512];
        new Random(1234).NextBytes(payload);
        using var source = Message.From(payload);
        payload[0] ^= 0xFF;

        using Message copy = source.Copy();

        Assert.True(source.RefCount >= 2);
        Assert.True(copy.RefCount >= 2);
        Assert.True(copy.AsReadOnlySpan().SequenceEqual(source.AsReadOnlySpan()));
        Assert.NotEqual(payload[0], source.AsReadOnlySpan()[0]);
    }

    [Fact]
    public void message_from_message_copies_payload()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var source = Message.From("copy-source"u8);
        using Message copy = Message.From(source);

        Assert.True(copy.AsReadOnlySpan().SequenceEqual("copy-source"u8));
    }

    [Fact]
    public void message_string_helpers_round_trip_utf8_payload()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        const string payload = "dotnet-메시지";
        using Message message = Message.From(payload, Encoding.UTF8);

        Assert.Equal(payload, message.GetString());
        Assert.True(message.AsReadOnlySpan().SequenceEqual(Encoding.UTF8.GetBytes(payload)));
    }

    [Fact]
    public void message_allocate_exposes_writable_owned_payload()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using Message message = Message.Allocate(3);
        Assert.False(message.IsEmpty);
        Span<byte> span = message.AsSpan();
        span[0] = 0x01;
        span[1] = 0x02;
        span[2] = 0x03;

        Assert.Equal(new byte[] { 0x01, 0x02, 0x03 }, message.ToArray());
    }

    [Fact]
    public void message_span_access_is_bounded_by_message_lifetime()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        Message message = Message.Allocate(4);
        Span<byte> span = message.AsSpan();
        "view"u8.CopyTo(span);
        byte[] snapshot = message.ToArray();

        message.Dispose();

        Assert.Equal("view"u8.ToArray(), snapshot);
        Assert.Throws<ObjectDisposedException>(() => message.AsSpan());
        Assert.Throws<ObjectDisposedException>(() => message.AsReadOnlySpan());
    }

}
