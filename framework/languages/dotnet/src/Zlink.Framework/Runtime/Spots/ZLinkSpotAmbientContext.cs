namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkSpotAmbientContext
{
    private static readonly AsyncLocal<IZLinkCurrentSpotActivation?> Current = new();

    public static IZLinkCurrentSpotActivation? CurrentOrDefault => Current.Value;

    public static IZLinkCurrentSpotActivation RequireCurrent()
    {
        return Current.Value
               ?? throw new InvalidOperationException(
                   "IZLinkSpotOutbound can only be used inside an active SPOT callback.");
    }

    public static IDisposable Push(IZLinkCurrentSpotActivation activation)
    {
        var previous = Current.Value;
        Current.Value = activation;
        return new Revert(previous);
    }

    private sealed class Revert(IZLinkCurrentSpotActivation? previous) : IDisposable
    {
        public void Dispose()
        {
            Current.Value = previous;
        }
    }
}