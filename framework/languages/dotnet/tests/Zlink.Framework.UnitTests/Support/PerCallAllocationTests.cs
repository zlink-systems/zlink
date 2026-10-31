namespace Zlink.Framework.UnitTests;

// The allocation tests compare PerCallAllocation results strictly, so the helper itself
// must report a per-call cost and must not report one where there is none.
public sealed class PerCallAllocationTests
{
    private static object? _sink;

    [Fact]
    public void Reports_zero_for_a_call_that_does_not_allocate()
    {
        var value = 0;
        Assert.Equal(0, PerCallAllocation.MinWindow(() => value++, callsPerWindow: 64, warmup: 8));
    }

    [Fact]
    public void Reports_a_per_call_allocation_in_every_window()
    {
        var bytes = PerCallAllocation.MinWindow(
            () => _sink = new byte[64],
            callsPerWindow: 64,
            warmup: 8
        );
        Assert.True(bytes >= 64 * 64, $"measured {bytes} bytes for 64 allocations of 64 bytes");
    }

    [Fact]
    public void Excludes_a_one_time_allocation_from_the_minimum()
    {
        var first = true;
        var bytes = PerCallAllocation.MinWindow(
            _ =>
            {
                if (first)
                    _sink = new byte[4096];
                first = false;
            },
            callsPerWindow: 64
        );
        Assert.Equal(0, bytes);
    }

    [Theory]
    [InlineData("single", 0)]
    [InlineData("single", 16 * 1024)]
    [InlineData("pair", 0)]
    [InlineData("pair", 16 * 1024)]
    [InlineData("async", 0)]
    [InlineData("async", 16 * 1024)]
    public async Task Measures_every_call_without_gc_accounting_noise(string mode, int payloadSize)
    {
        const int calls = 128;
        var before = GC.GetAllocatedBytesForCurrentThread();
        if (payloadSize != 0)
            _sink = new byte[payloadSize];
        var expected = (GC.GetAllocatedBytesForCurrentThread() - before) * calls;

        void Allocate()
        {
            Assert.True(
                System.Runtime.GCSettings.LatencyMode == System.Runtime.GCLatencyMode.NoGCRegion
            );
            if (payloadSize != 0)
                _sink = new byte[payloadSize];
        }

        switch (mode)
        {
            case "single":
                Assert.Equal(expected, PerCallAllocation.MinWindow(Allocate, calls));
                break;
            case "pair":
                var pair = PerCallAllocation.MinWindowPair(_ => Allocate(), _ => Allocate(), calls);
                Assert.Equal(expected, pair.First);
                Assert.Equal(expected, pair.Second);
                break;
            case "async":
                Assert.Equal(
                    expected,
                    await PerCallAllocation.MinWindowAsync(
                        () =>
                        {
                            Allocate();
                            return ValueTask.CompletedTask;
                        },
                        calls
                    )
                );
                break;
        }
    }

    [Theory]
    [InlineData("single")]
    [InlineData("pair")]
    [InlineData("async")]
    public async Task Rejects_a_collection_inside_the_measurement(string mode)
    {
        switch (mode)
        {
            case "single":
                Assert.Throws<InvalidOperationException>(() =>
                    PerCallAllocation.MinWindow(() => GC.Collect(), 1)
                );
                break;
            case "pair":
                Assert.Throws<InvalidOperationException>(() =>
                    PerCallAllocation.MinWindowPair(_ => { }, _ => GC.Collect(), 1)
                );
                break;
            case "async":
                await Assert.ThrowsAsync<InvalidOperationException>(() =>
                    PerCallAllocation.MinWindowAsync(
                        () =>
                        {
                            GC.Collect();
                            return ValueTask.CompletedTask;
                        },
                        1
                    )
                );
                break;
        }
        Assert.Equal(0, PerCallAllocation.MinWindow(() => { }, 1));
    }

    [Fact]
    public void Releases_the_region_when_the_action_throws()
    {
        var failure = new InvalidOperationException("action failed");
        Assert.Same(
            failure,
            Assert.Throws<InvalidOperationException>(() =>
                PerCallAllocation.MinWindow(() => throw failure, 1)
            )
        );
        Assert.Equal(0, PerCallAllocation.MinWindow(() => { }, 1));
    }
}
