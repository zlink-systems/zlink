# Network Sync FPS demo

The native Unity client joins a two-player duel over WebSocket STREAM. The Game
view shows interpolated player positions, health and the server snapshot ticks.

1. From this directory, run `dotnet build Managed~/Demo.Managed.csproj -c Release`.
   This builds the demo's managed client and stages its STREAM connector and
   dependencies in `Assets/Plugins`. Unity imports Network Sync from the sibling
   `com.zlink.network-sync` package.
2. From
   [the FPS template](../../../../../framework/languages/dotnet/templates/fps-entry-lobby/my-game),
   run `./run-server.sh --network-sync` (or `run-server.cmd --network-sync`).
   Copy the STREAM endpoint printed by the foreground runner.
3. Open this directory in Unity 6000.0.83f1 and open `Assets/Duel.unity`. Enter Play
   Mode. Run a second native instance from the same scene, for example by building
   a standalone player. Enter the same endpoint and distinct player IDs in both
   instances and select **Join duel**.
4. Use WASD to move. Click the play area to aim and fire. The server applies both on
   its simulation ticks. The view uses the two received states and ratio returned
   by the Network Sync client.

The Unity Test Runner's EditMode tab includes
`Zlink.NetworkSync.Tests.ConsumerTests.AttachesToManagedConnector`.
