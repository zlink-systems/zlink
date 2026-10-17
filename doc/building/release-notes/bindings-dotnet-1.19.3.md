[English](./bindings-dotnet-1.19.3.md) | [한국어](./bindings-dotnet-1.19.3.ko.md)

# ZLink .NET binding 1.19.3 release notes

Uses Core 1.19.0.

## Changes

- Through 1.19.2, when a send or request submission failed immediately, the caller received the exception, but the internal completion `Task` that was never returned was also left faulted with the same exception. After garbage collection this raised `TaskScheduler.UnobservedTaskException`, for example when sending to a missing peer on a Router. The binding now observes the exception of the unreturned internal `Task`; the caller still receives the same exception, and no unobserved exception remains. There is no public API change (#2073).
