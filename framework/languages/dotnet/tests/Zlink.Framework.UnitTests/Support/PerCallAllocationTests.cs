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
}
