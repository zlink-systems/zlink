namespace Zlink.Framework.Runtime.Messaging;

internal interface IZLinkMessagePartSerializer
{
    Message SerializePart(object value, Type type);
}
