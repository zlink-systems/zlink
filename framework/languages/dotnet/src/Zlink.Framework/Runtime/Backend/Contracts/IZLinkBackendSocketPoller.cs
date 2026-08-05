namespace Zlink.Framework.Runtime.Backend.Contracts;

// Framework receive loops use this seam to wait for native socket readiness.
// The .NET backend implementation owns the public Systems.Zlink poller and the
// socket registration; callers only receive the readiness flags they need.
internal interface IZLinkBackendSocketPoller : IDisposable
{
    PollEventFlags Wait(TimeSpan timeout);
}
