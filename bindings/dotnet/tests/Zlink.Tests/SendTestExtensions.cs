namespace Systems.Zlink.Tests;

internal static class SendTestExtensions
{
    internal static bool TrySubmit(this RoutedSendSubmitOperation operation,
        SendFlags flags)
    {
        try
        {
            operation.Submit(flags);
            return true;
        }
        catch (ZlinkSubmitException error) when (
            error.Result == ZlinkSubmitException.ErrorCode.Backpressured)
        {
            return false;
        }
    }
}
