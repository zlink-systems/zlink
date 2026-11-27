#nullable enable
using System;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using UnityEngine;

namespace Zlink.NetworkSync.FpsDemo
{
    public sealed class FpsDemo : MonoBehaviour
    {
        private readonly CancellationTokenSource lifetime = new();
        private string endpoint = "";
        private string playerId = "unity-" + Guid.NewGuid().ToString("N").Substring(0, 8);
        private string? failure;
        private DemoClient? client;
        private Task operation = Task.CompletedTask;
        private Point? queuedShot;
        private Point? origin;
        private Vector2? aimOrigin;

        private static Rect PlayArea => new(12, 168, Mathf.Min(Screen.width - 24, 640), Screen.height - 180);

        private void Awake() => Application.runInBackground = true;

        private void Update()
        {
            if (client is null || failure is not null)
                return;
            if (Input.GetMouseButtonDown(0) && aimOrigin is { } position)
            {
                var cursor = new Vector2(Input.mousePosition.x, Screen.height - Input.mousePosition.y);
                if (PlayArea.Contains(cursor))
                {
                    var offset = cursor - position;
                    queuedShot = new Point(offset.x, 0, -offset.y);
                }
            }
            if (!operation.IsCompleted)
                return;

            var move = new Point(Axis(KeyCode.D, KeyCode.A), 0, Axis(KeyCode.W, KeyCode.S));
            var shot = queuedShot;
            queuedShot = null;
            operation = ObserveAsync(client.UpdateAsync(move, shot, lifetime.Token));
        }

        private static int Axis(KeyCode positive, KeyCode negative) =>
            (Input.GetKey(positive) ? 1 : 0) - (Input.GetKey(negative) ? 1 : 0);

        private async Task ObserveAsync(Task task)
        {
            try
            {
                await task;
            }
            catch (OperationCanceledException) when (lifetime.IsCancellationRequested) { }
            catch (Exception error)
            {
                failure = error.Message;
                Debug.LogException(error);
            }
        }

        private void OnGUI()
        {
            SnapshotPair<RoomView> pair = default;
            var active = client;
            var ready = active is not null && operation.IsCompleted && active.TryGetInterpolation(out pair);
            GUILayout.BeginArea(new Rect(12, 10, Mathf.Min(Screen.width - 24, 640), 154));
            if (client is null)
            {
                GUILayout.Label("Server STREAM endpoint");
                endpoint = GUILayout.TextField(endpoint);
                GUILayout.Label("Player ID");
                playerId = GUILayout.TextField(playerId);
                if (GUILayout.Button("Join duel"))
                {
                    if (Uri.TryCreate(endpoint, UriKind.Absolute, out var address)
                        && (address.Scheme is "ws" or "wss") && !string.IsNullOrWhiteSpace(playerId))
                    {
                        failure = null;
                        client = new DemoClient(address, playerId);
                        operation = ObserveAsync(client.JoinAsync(lifetime.Token));
                    }
                    else
                        failure = "Enter a WebSocket STREAM endpoint and a player ID.";
                }
            }
            else
                GUILayout.Label("WASD to move. Click in the play area to aim and fire.");
            GUILayout.Label(failure ?? (ready ? "In match" : active?.Status) ?? "");
            GUILayout.EndArea();

            if (active is null || !ready)
            {
                aimOrigin = null;
                return;
            }

            var self = pair.To.Players.FirstOrDefault(player => player.PlayerId == active.PlayerId);
            origin ??= self?.Position;
            if (origin is null)
                return;
            GUI.Box(PlayArea, pair.To.Phase == MatchPhase.Finished
                ? $"Winner: {pair.To.Winner}"
                : $"Snapshot ticks {pair.FromTick}–{pair.ToTick}");
            GUI.BeginGroup(PlayArea);
            foreach (var player in pair.To.Players)
            {
                var from = pair.From.Players.FirstOrDefault(candidate => candidate.PlayerId == player.PlayerId);
                var before = Project(from?.Position ?? player.Position);
                var after = Project(player.Position);
                var center = Vector2.Lerp(before, after, (float)pair.Ratio) - PlayArea.position;
                if (player.PlayerId == active.PlayerId)
                    aimOrigin = center + PlayArea.position;
                GUI.Box(new Rect(center.x - 10, center.y - 10, 20, 20), player.PlayerId == active.PlayerId ? "+" : "");
                GUI.Label(new Rect(center.x + 12, center.y - 10, 194, 20), $"{player.PlayerId}  {player.Hp} HP");
            }
            GUI.EndGroup();
        }

        private Vector2 Project(Point point)
        {
            const float pixelsPerUnit = 14;
            var center = origin.GetValueOrDefault();
            return PlayArea.center + new Vector2(
                (float)(point.X - center.X) * pixelsPerUnit,
                (float)(center.Z - point.Z) * pixelsPerUnit);
        }

        private async void OnDestroy()
        {
            lifetime.Cancel();
            try
            {
                await operation;
                if (client is not null)
                    await client.DisposeAsync();
            }
            catch (Exception error)
            {
                Debug.LogException(error);
            }
            finally
            {
                lifetime.Dispose();
            }
        }
    }
}
