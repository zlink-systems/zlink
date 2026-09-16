using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace Systems.Zlink.Stream.Connector.Runtime
{
    /// <summary>
    ///     Minimal JSON writer and reader for the jslib boundary.
    /// </summary>
    /// <remarks>
    ///     Unity ships neither System.Text.Json nor Newtonsoft.Json by default, and
    ///     UnityEngine.JsonUtility cannot represent the string-to-string metadata map. A
    ///     connector adapter must not force a serializer dependency on the game, so the
    ///     boundary uses this reader/writer pair instead. It covers only the shapes this
    ///     package exchanges: objects of strings, numbers, booleans, nulls and nested
    ///     objects. Application payloads never pass through here - they cross the boundary
    ///     as raw bytes.
    /// </remarks>
    internal static class ZlinkStreamJson
    {
        internal sealed class Writer
        {
            private readonly StringBuilder _builder = new StringBuilder();
            private bool _needsComma;

            public Writer StartObject()
            {
                Separate();
                _builder.Append('{');
                _needsComma = false;
                return this;
            }

            public Writer EndObject()
            {
                _builder.Append('}');
                _needsComma = true;
                return this;
            }

            public Writer Name(string name)
            {
                Separate();
                AppendString(name);
                _builder.Append(':');
                _needsComma = false;
                return this;
            }

            public Writer String(string name, string value)
            {
                if (value is null) return this;
                Name(name);
                AppendString(value);
                _needsComma = true;
                return this;
            }

            public Writer Number(string name, double value)
            {
                Name(name);
                _builder.Append(value.ToString("R", CultureInfo.InvariantCulture));
                _needsComma = true;
                return this;
            }

            public Writer Bool(string name, bool value)
            {
                Name(name);
                _builder.Append(value ? "true" : "false");
                _needsComma = true;
                return this;
            }

            public override string ToString()
            {
                return _builder.ToString();
            }

            private void Separate()
            {
                if (_needsComma) _builder.Append(',');
            }

            private void AppendString(string value)
            {
                _builder.Append('"');
                foreach (var character in value)
                {
                    switch (character)
                    {
                        case '"': _builder.Append("\\\""); break;
                        case '\\': _builder.Append("\\\\"); break;
                        case '\b': _builder.Append("\\b"); break;
                        case '\f': _builder.Append("\\f"); break;
                        case '\n': _builder.Append("\\n"); break;
                        case '\r': _builder.Append("\\r"); break;
                        case '\t': _builder.Append("\\t"); break;
                        default:
                            if (character < ' ')
                                _builder.Append("\\u").Append(((int)character).ToString("x4", CultureInfo.InvariantCulture));
                            else
                                _builder.Append(character);
                            break;
                    }
                }

                _builder.Append('"');
            }
        }

        internal sealed class Node
        {
            private readonly Dictionary<string, Node> _members;

            private Node(Dictionary<string, Node> members, string text, double? number, bool? flag)
            {
                _members = members;
                Text = text;
                Number = number;
                Flag = flag;
            }

            public string Text { get; }

            public double? Number { get; }

            public bool? Flag { get; }

            public static Node Object(Dictionary<string, Node> members)
            {
                return new Node(members, null, null, null);
            }

            public static Node Scalar(string text, double? number, bool? flag)
            {
                return new Node(null, text, number, flag);
            }

            public Node Member(string name)
            {
                if (_members is null) return null;
                return _members.TryGetValue(name, out var node) ? node : null;
            }

            public IEnumerable<KeyValuePair<string, Node>> Members()
            {
                return _members ?? (IEnumerable<KeyValuePair<string, Node>>)Array.Empty<KeyValuePair<string, Node>>();
            }

            public string TextOf(string name)
            {
                var node = Member(name);
                return node?.Text;
            }

            public int IntOf(string name, int fallback)
            {
                var node = Member(name);
                return node?.Number is null ? fallback : (int)node.Number.Value;
            }
        }

        public static Node Parse(string text)
        {
            if (string.IsNullOrEmpty(text)) return Node.Object(new Dictionary<string, Node>(StringComparer.Ordinal));
            var index = 0;
            var node = ParseValue(text, ref index);
            return node ?? Node.Object(new Dictionary<string, Node>(StringComparer.Ordinal));
        }

        private static Node ParseValue(string text, ref int index)
        {
            SkipWhitespace(text, ref index);
            if (index >= text.Length) return null;
            var character = text[index];
            if (character == '{') return ParseObject(text, ref index);
            if (character == '[') { SkipArray(text, ref index); return Node.Scalar(null, null, null); }
            if (character == '"') return Node.Scalar(ParseString(text, ref index), null, null);
            if (StartsWith(text, index, "true")) { index += 4; return Node.Scalar(null, null, true); }
            if (StartsWith(text, index, "false")) { index += 5; return Node.Scalar(null, null, false); }
            if (StartsWith(text, index, "null")) { index += 4; return Node.Scalar(null, null, null); }
            return Node.Scalar(null, ParseNumber(text, ref index), null);
        }

        private static Node ParseObject(string text, ref int index)
        {
            var members = new Dictionary<string, Node>(StringComparer.Ordinal);
            index += 1;
            SkipWhitespace(text, ref index);
            if (index < text.Length && text[index] == '}') { index += 1; return Node.Object(members); }
            while (index < text.Length)
            {
                SkipWhitespace(text, ref index);
                var name = ParseString(text, ref index);
                SkipWhitespace(text, ref index);
                if (index < text.Length && text[index] == ':') index += 1;
                var value = ParseValue(text, ref index);
                members[name] = value ?? Node.Scalar(null, null, null);
                SkipWhitespace(text, ref index);
                if (index < text.Length && text[index] == ',') { index += 1; continue; }
                if (index < text.Length && text[index] == '}') { index += 1; break; }
                break;
            }

            return Node.Object(members);
        }

        private static string ParseString(string text, ref int index)
        {
            if (index >= text.Length || text[index] != '"') return null;
            index += 1;
            var builder = new StringBuilder();
            while (index < text.Length)
            {
                var character = text[index++];
                if (character == '"') break;
                if (character != '\\')
                {
                    builder.Append(character);
                    continue;
                }

                if (index >= text.Length) break;
                var escape = text[index++];
                switch (escape)
                {
                    case '"': builder.Append('"'); break;
                    case '\\': builder.Append('\\'); break;
                    case '/': builder.Append('/'); break;
                    case 'b': builder.Append('\b'); break;
                    case 'f': builder.Append('\f'); break;
                    case 'n': builder.Append('\n'); break;
                    case 'r': builder.Append('\r'); break;
                    case 't': builder.Append('\t'); break;
                    case 'u':
                        if (index + 4 <= text.Length)
                        {
                            builder.Append((char)Convert.ToInt32(text.Substring(index, 4), 16));
                            index += 4;
                        }

                        break;
                    default: builder.Append(escape); break;
                }
            }

            return builder.ToString();
        }

        private static double ParseNumber(string text, ref int index)
        {
            var start = index;
            while (index < text.Length && "+-.eE0123456789".IndexOf(text[index]) >= 0) index += 1;
            var slice = text.Substring(start, index - start);
            return double.TryParse(slice, NumberStyles.Float, CultureInfo.InvariantCulture, out var value) ? value : 0d;
        }

        private static void SkipArray(string text, ref int index)
        {
            var depth = 0;
            while (index < text.Length)
            {
                var character = text[index];
                if (character == '"') { ParseString(text, ref index); continue; }
                index += 1;
                if (character == '[') depth += 1;
                else if (character == ']') { depth -= 1; if (depth == 0) return; }
            }
        }

        private static void SkipWhitespace(string text, ref int index)
        {
            while (index < text.Length && char.IsWhiteSpace(text[index])) index += 1;
        }

        private static bool StartsWith(string text, int index, string token)
        {
            return index + token.Length <= text.Length && string.CompareOrdinal(text, index, token, 0, token.Length) == 0;
        }
    }
}
