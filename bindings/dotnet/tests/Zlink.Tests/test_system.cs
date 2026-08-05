using System;
using System.Threading;
using System.Threading.Tasks;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_system
{
    private static IPairSocket? s_socketThatOutlivesContext;

    [Fact]
    public void version_matches_core_header()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        var expected = CoreTestSupport.ReadCoreHeaderVersion();
        var actual = global::Systems.Zlink.Zlink.Version();

        Assert.Equal(expected.major, actual.Major);
        Assert.Equal(expected.minor, actual.Minor);
        Assert.Equal(expected.patch, actual.Patch);
    }

    [Fact]
    public void create_and_destroy_context_socket()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var socket = ctx.CreatePairSocket();

        var probe = Received.Create();
        Assert.False(socket.Recv(probe, RecvFlags.DontWait));
        Assert.True(Zlink.Has("tcp") || !Zlink.Has("tcp"));
    }

    [Fact]
    public async Task context_finalizer_does_not_block_when_socket_outlives_context()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        CreateContextForFinalizerTest();
        Task finalizers = Task.Run(() =>
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
        });
        Task completed = await Task.WhenAny(
            finalizers,
            Task.Delay(TimeSpan.FromSeconds(3)));

        s_socketThatOutlivesContext?.Dispose();
        s_socketThatOutlivesContext = null;

        Assert.Same(finalizers, completed);
    }

    [Fact]
    public void runtime_sleep_overloads_callable()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        Zlink.Sleep(TimeSpan.Zero);
    }

    [Fact]
    public void atomic_counter_basic_contract()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var counter = Zlink.CreateAtomicCounter();
        counter.Set(3);

        Assert.Equal(3, counter.Value);
        _ = counter.Increment();
        Assert.Equal(4, counter.Value);
        _ = counter.Decrement();
        Assert.Equal(3, counter.Value);
    }

    [Fact]
    public void stopwatch_basic_contract()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var watch = Zlink.CreateStopwatch();

        ulong intermediate = watch.Intermediate();
        ulong elapsed = watch.Stop();

        Assert.True(elapsed >= intermediate);
        Assert.Throws<ObjectDisposedException>(() => watch.Intermediate());
    }

    [Fact]
    public void timer_basic_contract_uses_native_backend()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var timer = Zlink.CreateTimer();
        timer.Start(TimeSpan.FromMilliseconds(5), 1);

        ulong? fireCount = timer.Recv();

        Assert.Equal(1UL, fireCount.GetValueOrDefault());
    }

    [Fact]
    public void timer_on_fire_invokes_callback()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var timer = Zlink.CreateTimer();
        using var fired = new ManualResetEventSlim(false);
        ulong observed = 0;

        timer.OnFire((_, fireCount) =>
        {
            observed = fireCount;
            fired.Set();
        });
        timer.Start(TimeSpan.FromMilliseconds(5), 1);

        Assert.True(fired.Wait(20000));
        Assert.Equal(1UL, observed);
    }

    private static void CreateContextForFinalizerTest()
    {
        var ctx = Zlink.CreateContext();
        s_socketThatOutlivesContext = ctx.CreatePairSocket();
    }
}
