using System;

namespace EngineLobby
{
    [Serializable]
    public sealed class PingReq
    {
        public string sentAtUnixMs;

        public PingReq(string sentAtUnixMs)
        {
            this.sentAtUnixMs = sentAtUnixMs;
        }
    }

    [Serializable]
    public sealed class PingRes
    {
        public string sentAtUnixMs;
    }

    [Serializable]
    public sealed class JoinReq
    {
        public string name;

        public JoinReq(string name)
        {
            this.name = name;
        }
    }

    [Serializable]
    public sealed class JoinRes
    {
        public string actorId;
        public string name;
    }

    [Serializable]
    public sealed class ChatMsg
    {
        public string text;

        public ChatMsg(string text)
        {
            this.text = text;
        }
    }

    [Serializable]
    public sealed class ChatNotify
    {
        public string actorId;
        public string name;
        public string text;
    }
}
