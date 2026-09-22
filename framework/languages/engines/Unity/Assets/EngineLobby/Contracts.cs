using System;

namespace EngineLobby
{
    [Serializable]
    public sealed class Ping
    {
        public string sentAtUnixMs;

        public Ping(string sentAtUnixMs)
        {
            this.sentAtUnixMs = sentAtUnixMs;
        }
    }

    [Serializable]
    public sealed class Pong
    {
        public string sentAtUnixMs;
    }

    [Serializable]
    public sealed class Join
    {
        public string name;

        public Join(string name)
        {
            this.name = name;
        }
    }

    [Serializable]
    public sealed class Joined
    {
        public string actorId;
        public string name;
    }

    [Serializable]
    public sealed class Chat
    {
        public string text;

        public Chat(string text)
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
