// Drives com.zlink.stream-connector.webgl inside a real Unity WebGL player.
//
// This is not a sample. It exists to answer four questions that only a Unity
// build can answer, and it adds nothing beyond what they need:
//
//   1. IL2CPP codegen - does the [AOT.MonoPInvokeCallback] reverse call survive
//      the C# -> WASM translation? Every event the connector delivers arrives
//      through it, so a connect that completes already proves it.
//   2. asmdef platform gating - is the assembly that got compiled into the
//      player the WebGL-only one? Reported as `adapterAssembly`.
//   3. UPM import - Unity read the package at all, or this assembly would not
//      compile.
//   4. .jspre / .jslib linking - reported as `linkedPlugins`, asked of the
//      running player rather than of the built file.
//
// The entry point is a [RuntimeInitializeOnLoadMethod] so the build needs no
// authored scene and no serialized component reference. That matters because
// this assembly is WebGL-only: the Editor cannot see the type, so an Editor
// script could not add it to a scene.
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using Systems.Zlink.Stream.Connector.Contracts;
using UnityEngine;

namespace Zlink.Verification
{
    public sealed class ZlinkWebGlAdapterCheck : MonoBehaviour
    {
        private const string PushPacket = "EchoPush";
        private const string RequestPacket = "EchoReq";
        private const float StepTimeoutSeconds = 30f;

        private readonly List<string> _steps = new List<string>();
        private readonly List<string> _pushes = new List<string>();

        private IZlinkStreamConnector _connector;
        private IDisposable _subscription;
        private bool _pumping;
        private bool _dispatchInFlight;
        private string _pumpError;

        [DllImport("__Internal")]
        private static extern void ZlinkVerificationReport(string json);

        [DllImport("__Internal")]
        private static extern int ZlinkVerificationLinkedPlugins();

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
        private static void Bootstrap()
        {
            var host = new GameObject("ZlinkWebGlAdapterCheck");
            DontDestroyOnLoad(host);
            host.AddComponent<ZlinkWebGlAdapterCheck>();
        }

        private async void Start()
        {
            try
            {
                await RunAsync();
            }
            catch (Exception error)
            {
                Fail(error.ToString());
            }
        }

        // The README's pump: Dispatch from Update on the main thread. Guarded
        // against overlap because Dispatch is awaited across frames, and a second
        // one started from the next Update would drain the same queue.
        private void Update()
        {
            if (!_pumping || _dispatchInFlight || _connector == null) return;
            _dispatchInFlight = true;
            PumpOnce();
        }

        private async void PumpOnce()
        {
            try
            {
                await _connector.Dispatch.Async();
            }
            catch (Exception error)
            {
                if (_pumpError == null) _pumpError = error.ToString();
            }
            finally
            {
                _dispatchInFlight = false;
            }
        }

        private async Task RunAsync()
        {
            var endpoint = QueryValue(Application.absoluteURL, "endpoint");
            if (string.IsNullOrEmpty(endpoint))
            {
                Fail("no ?endpoint= in " + Application.absoluteURL);
                return;
            }

            _connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri(endpoint),
                DispatchMode = ZlinkStreamDispatchMode.Manual,
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
            });
            Step("created state=" + _connector.State);

            await _connector.Connect.Async();
            Step("connected isConnected=" + _connector.IsConnected + " state=" + _connector.State);

            var reply = await _connector
                .Request(Encode("unity-request"))
                .PacketName(RequestPacket)
                .Timeout(TimeSpan.FromSeconds(StepTimeoutSeconds))
                .Async();
            var replyText = Decode(reply);
            Step("replied " + replyText);
            if (replyText.IndexOf("unity-request", StringComparison.Ordinal) < 0)
            {
                Fail("reply did not carry the request value: " + replyText);
                return;
            }

            // Registered after the request, so the push the request produced is
            // already in the unread history and this handler only sees the next
            // one. Same ordering as the emscripten-level test.
            _subscription = _connector.On(PushPacket, (message, _) =>
            {
                _pushes.Add(Decode(message.Payload));
                return default;
            });
            _pumping = true;

            await _connector.Send(Encode("unity-send")).PacketName(RequestPacket).Async();
            Step("sent");

            var deadline = Time.realtimeSinceStartup + StepTimeoutSeconds;
            while (_pushes.Count == 0 && _pumpError == null && Time.realtimeSinceStartup < deadline)
            {
                await Task.Yield();
            }

            _pumping = false;
            if (_pumpError != null)
            {
                Fail("Dispatch from Update failed: " + _pumpError);
                return;
            }

            if (_pushes.Count == 0)
            {
                Fail("no push reached the handler within " + StepTimeoutSeconds + "s");
                return;
            }

            Step("dispatched " + _pushes[0]);
            if (_pushes[0].IndexOf("unity-send", StringComparison.Ordinal) < 0)
            {
                Fail("the handler saw the wrong push: " + _pushes[0]);
                return;
            }

            _subscription.Dispose();
            await _connector.Close.Async();
            Step("closed state=" + _connector.State);

            if (_connector.State != ZlinkStreamConnectionState.Closed)
            {
                Fail("close left the connector in " + _connector.State);
                return;
            }

            Report(true, null);
        }

        private static ZlinkStreamEncodedPayload Encode(string value)
        {
            var json = "{\"value\":\"" + value + "\"}";
            return new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Json, Encoding.UTF8.GetBytes(json));
        }

        private static string Decode(ZlinkStreamEncodedPayload payload)
        {
            return Encoding.UTF8.GetString(payload.Payload.ToArray());
        }

        private void Step(string text)
        {
            _steps.Add(text);
            Debug.Log("ZLINK-VERIFY step: " + text);
        }

        private void Fail(string reason)
        {
            Report(false, reason);
        }

        private void Report(bool ok, string reason)
        {
            var linked = 0;
            try
            {
                linked = ZlinkVerificationLinkedPlugins();
            }
            catch (Exception error)
            {
                reason = (reason ?? string.Empty) + " | linked-plugins probe failed: " + error.Message;
            }

            var json = new StringBuilder();
            json.Append("{\"ok\":").Append(ok ? "true" : "false");
            json.Append(",\"unityVersion\":").Append(Quote(Application.unityVersion));
            json.Append(",\"adapterAssembly\":")
                .Append(Quote(typeof(ZlinkStreamConnectorFactory).Assembly.GetName().Name));
            json.Append(",\"linkedPlugins\":{\"runtime\":").Append((linked & 1) != 0 ? "true" : "false")
                .Append(",\"bundle\":").Append((linked & 2) != 0 ? "true" : "false").Append('}');
            json.Append(",\"steps\":[");
            for (var index = 0; index < _steps.Count; index += 1)
            {
                if (index > 0) json.Append(',');
                json.Append(Quote(_steps[index]));
            }

            json.Append("],\"reason\":").Append(reason == null ? "null" : Quote(reason));
            json.Append('}');
            ZlinkVerificationReport(json.ToString());
        }

        private static string Quote(string value)
        {
            var text = new StringBuilder("\"");
            foreach (var character in value)
            {
                switch (character)
                {
                    case '"': text.Append("\\\""); break;
                    case '\\': text.Append("\\\\"); break;
                    case '\n': text.Append("\\n"); break;
                    case '\r': text.Append("\\r"); break;
                    case '\t': text.Append("\\t"); break;
                    default:
                        if (character < ' ') text.Append("\\u").Append(((int)character).ToString("x4"));
                        else text.Append(character);
                        break;
                }
            }

            return text.Append('"').ToString();
        }

        private static string QueryValue(string url, string key)
        {
            if (string.IsNullOrEmpty(url)) return null;
            var start = url.IndexOf('?');
            if (start < 0) return null;

            var query = url.Substring(start + 1);
            var fragment = query.IndexOf('#');
            if (fragment >= 0) query = query.Substring(0, fragment);

            foreach (var pair in query.Split('&'))
            {
                var separator = pair.IndexOf('=');
                if (separator <= 0) continue;
                if (!string.Equals(pair.Substring(0, separator), key, StringComparison.Ordinal)) continue;
                return Uri.UnescapeDataString(pair.Substring(separator + 1));
            }

            return null;
        }
    }
}
