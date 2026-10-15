using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Xunit;

namespace Systems.Zlink.Tests;

// #1835: Core stores payloads of up to 29 bytes inside zlink_msg_t itself.
// The payload views a Message hands out must therefore come from storage the
// GC never moves, for as long as the message is undisposed.
public sealed class test_message_gc_lifetime
{
    private const int Repetitions = 1000;
    private const int MessagesPerRound = 64;
    private const int WorkerCount = 4;

    [Theory]
    [InlineData(8)]
    [InlineData(29)]
    [InlineData(30)]
    public void payload_address_is_stable_across_compacting_gc(int size)
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        byte[] payload = Payload(size);
        int moved = 0;
        // A fresh thread starts with an empty thread-static wrapper pool, so
        // every message below is a newly allocated, movable managed wrapper.
        var thread = new Thread(() => moved = CountMovedPayloads(payload));
        thread.Start();
        thread.Join();
        Assert.Equal(0, moved);
    }

    private static unsafe int CountMovedPayloads(byte[] payload)
    {
        int moved = 0;
        var messages = new Message[MessagesPerRound];
        var addresses = new nint[MessagesPerRound];
        for (int iteration = 0; iteration < Repetitions / 50; iteration++)
        {
            for (int i = 0; i < messages.Length; i++)
            {
                // Dead garbage in front of each wrapper gives compaction a
                // reason to slide the wrapper.
                _ = new byte[256];
                messages[i] = Message.From(payload);
                addresses[i] = (nint)Unsafe.AsPointer(
                    ref MemoryMarshal.GetReference(messages[i].AsSpan()));
            }

            GC.Collect(2, GCCollectionMode.Forced, blocking: true,
                compacting: true);

            for (int i = 0; i < messages.Length; i++)
            {
                nint current = (nint)Unsafe.AsPointer(
                    ref MemoryMarshal.GetReference(messages[i].AsSpan()));
                if (current != addresses[i])
                {
                    // Do not touch the stale address: it now belongs to the GC.
                    moved++;
                }
                else if (!new ReadOnlySpan<byte>((void*)addresses[i],
                             payload.Length).SequenceEqual(payload))
                {
                    moved++;
                }

                messages[i].Dispose();
            }
        }

        return moved;
    }

    [Theory]
    [InlineData(8)]
    [InlineData(29)]
    [InlineData(30)]
    public void payload_round_trips_during_concurrent_compacting_gc(int size)
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        byte[] payload = Payload(size);
        long mismatches = 0;
        int running = WorkerCount;
        var collector = new Thread(() =>
        {
            // Bounded by the workers' fixed iteration count, not by time.
            while (Volatile.Read(ref running) > 0)
                GC.Collect(0, GCCollectionMode.Forced, blocking: true,
                    compacting: true);
        });
        Thread[] workers = Enumerable.Range(0, WorkerCount).Select(_ =>
            new Thread(() =>
            {
                var garbage = new List<byte[]>();
                var copy = new byte[payload.Length];
                for (int i = 0; i < Repetitions; i++)
                {
                    for (int j = 0; j < MessagesPerRound; j++)
                    {
                        garbage.Add(new byte[48]);
                        using Message message = Message.From(payload);
                        if (!message.AsReadOnlySpan().SequenceEqual(payload))
                            Interlocked.Increment(ref mismatches);
                        if (!message.TryCopyTo(copy, out int written)
                            || written != payload.Length
                            || !copy.AsSpan().SequenceEqual(payload))
                            Interlocked.Increment(ref mismatches);
                    }

                    garbage.Clear();
                }

                Interlocked.Decrement(ref running);
            })).ToArray();
        collector.Start();
        foreach (Thread worker in workers)
            worker.Start();
        foreach (Thread worker in workers)
            worker.Join();
        collector.Join();
        Assert.Equal(0, Interlocked.Read(ref mismatches));
    }

    [Fact]
    public void dispose_pool_and_finalizer_release_native_storage_once()
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        // A pool-owning thread that ends leaves its pooled wrappers to the
        // finalizer; leaked owned messages reach it with a live payload.
        var thread = new Thread(() =>
        {
            for (int i = 0; i < Repetitions; i++)
            {
                Message.Allocate(i % 64).Dispose();
                LeakOwnedMessage(i % 64);
                Message owned = Message.From(Payload(i % 64));
                owned.Dispose();
                owned.Dispose();
                Assert.Throws<ObjectDisposedException>(() => owned.AsSpan());
            }
        });
        thread.Start();
        thread.Join();
        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void LeakOwnedMessage(int size)
    {
        _ = Message.From(Payload(size));
    }

    private static byte[] Payload(int size)
    {
        return Enumerable.Range(0, size).Select(i => (byte)(0x11 + i))
            .ToArray();
    }
}
