using Zlink.Framework.ContractTests.Support;

namespace Zlink.Framework.ContractTests.Timers;

public sealed class TimerContracts
{
    [Fact]
    [ContractExample(typeof(IZLinkTimer))]
    public async Task Timer_contract_allows_spot_code_to_cancel_a_registered_timer()
    {
        await using var timer = new ExampleTimer();

        await timer.CancelAsync();

        Assert.True(timer.IsDisposed);
    }

    private sealed class ExampleTimer : IZLinkTimer
    {
        public bool IsDisposed { get; private set; }

        public ValueTask CancelAsync()
        {
            IsDisposed = true;
            return ValueTask.CompletedTask;
        }

        public ValueTask DisposeAsync()
        {
            IsDisposed = true;
            return ValueTask.CompletedTask;
        }
    }
}
