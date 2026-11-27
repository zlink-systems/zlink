# ZLink Network Sync for Unity

Install this UPM package in a native Unity project and include the `netstandard2.1`
`Zlink.Stream.Connector` assembly and its managed dependencies. Use the connector's
default JSON profile and Manual dispatch mode.

For a player build with managed stripping, preserve the reflected types in your
project's `Assets/link.xml`. Add the game DTOs alongside the two assemblies below.
[Unity managed code stripping](https://docs.unity3d.com/2022.3/Documentation/Manual/ManagedCodeStripping.html#LinkXMLAnnotation)

```xml
<linker>
  <assembly fullname="Zlink.NetworkSync" preserve="all" />
  <assembly fullname="Systems.Zlink.Stream.Connector" preserve="all" />
</linker>
```

Attach one client after the match has been joined and one Actor is bound to the
connector. Keep that client for the Actor's participation in the match. The
application owns joining, connecting, and disposing the connector. Dispose the sync
client when leaving or when the connection ends.

On each frame, finish the connector's `Dispatch.Async()` and await the client's
`Update()` on the same thread. `Update()` may send a clock probe during startup or
idle time. Configure the snapshot interval in the constructor if the server uses
a period other than 50 ms.

Submit held input and any one-shot actions with `SendInputAsync`. A return value of
`false` means the client is preparing or has already submitted that target tick.
Keep unsent actions for a later tick. A return value of `true` means the connector
send completed. Submission failures propagate to the caller.

`TryGetInterpolation` returns two states and a ratio, or the most recent state
with `IsHolding` set until the hold period expires. A return value of `false` means
there is no interpolation result for this frame. Treat the received states as
read-only.

For Unity EditMode tests, make `com.zlink.network-sync` testable in the consumer's
`Packages/manifest.json`. Run the `Zlink.NetworkSync.EditorTests` assembly in the
Unity Test Runner. From the ZLink checkout, run `ArtifactsPath="$PWD/.artifacts" dotnet test Tests/Managed~` from this
package to exercise the clock and the shared wire fixtures without Unity.
