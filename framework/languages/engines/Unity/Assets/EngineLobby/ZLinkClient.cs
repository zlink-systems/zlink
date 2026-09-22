using System;
using Systems.Zlink.Stream.Connector.Contracts;
using UnityEngine;
using UnityEngine.UI;

namespace EngineLobby
{
    public sealed class ZLinkClient : MonoBehaviour
    {
        [SerializeField]
        private string endpoint = "ws://127.0.0.1:22700";

        [SerializeField]
        private string playerName = "unity-player";

        [SerializeField]
        private string firstChat = "hello from Unity";

        private IZlinkStreamConnector _connector;
        private IDisposable _chatSubscription;
        private Text _statusText;
        private bool _pumping;
        private bool _dispatchInFlight;

        private void Awake()
        {
            var canvasObject = new GameObject("EngineLobbyCanvas");
            var canvas = canvasObject.AddComponent<Canvas>();
            canvas.renderMode = RenderMode.ScreenSpaceOverlay;
            canvasObject.AddComponent<CanvasScaler>();

            var textObject = new GameObject("EngineLobbyStatus");
            textObject.transform.SetParent(canvasObject.transform, false);
            _statusText = textObject.AddComponent<Text>();
            _statusText.font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");
            _statusText.fontSize = 24;
            _statusText.alignment = TextAnchor.MiddleCenter;
            _statusText.color = Color.white;
            _statusText.rectTransform.anchorMin = Vector2.zero;
            _statusText.rectTransform.anchorMax = Vector2.one;
            _statusText.rectTransform.offsetMin = Vector2.zero;
            _statusText.rectTransform.offsetMax = Vector2.zero;
            _statusText.text = "Engine Lobby: connecting";
        }

        private async void Start()
        {
            try
            {
                // --8<-- [start:handler]
                _connector = ZlinkStreamConnectorFactory.Create(
                    new ZlinkStreamConnectorOptions
                    {
                        Endpoint = new Uri(endpoint),
                        DispatchMode = ZlinkStreamDispatchMode.Manual,
                        PayloadCodec = new UnityJsonPayloadCodec(),
                        Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                        Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                    }
                );
                _chatSubscription = _connector.On<ChatNotify>(
                    (message, cancellationToken) =>
                    {
                        _statusText.text = $"{message.Payload.name}: {message.Payload.text}";
                        return default;
                    }
                );
                // --8<-- [end:handler]

                // --8<-- [start:connect]
                await _connector.Connect.Async();
                _pumping = true;
                var joined = await _connector.Request(new JoinReq(playerName)).Async<JoinRes>();
                _statusText.text = $"joined as {joined.name} ({joined.actorId})";
                await _connector.Send(new ChatMsg(firstChat)).Async();
                // --8<-- [end:connect]
            }
            catch (Exception error)
            {
                _statusText.text = $"Engine Lobby failed: {error.Message}";
                Debug.LogException(error, this);
            }
        }

        // --8<-- [start:pump]
        private void Update()
        {
            if (!_pumping || _dispatchInFlight || _connector == null)
                return;

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
                _statusText.text = $"Engine Lobby dispatch failed: {error.Message}";
                Debug.LogException(error, this);
            }
            finally
            {
                _dispatchInFlight = false;
            }
        }

        // --8<-- [end:pump]

        // --8<-- [start:lifecycle]
        private async void OnApplicationQuit()
        {
            _pumping = false;
            _chatSubscription?.Dispose();
            if (_connector == null)
                return;

            try
            {
                if (_connector.IsConnected)
                    await _connector.Close.Async();
                await _connector.DisposeAsync();
            }
            catch (Exception error)
            {
                Debug.LogException(error, this);
            }
        }
        // --8<-- [end:lifecycle]
    }
}
