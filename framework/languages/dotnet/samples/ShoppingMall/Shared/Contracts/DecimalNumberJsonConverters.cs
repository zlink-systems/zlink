using System.Text.Json;
using System.Text.Json.Serialization;

namespace ShoppingMall.Shared.Contracts;

public sealed class DecimalNumberJsonConverter : JsonConverter<decimal>
{
    public override decimal Read(
        ref Utf8JsonReader reader,
        Type typeToConvert,
        JsonSerializerOptions options)
    {
        if (reader.TokenType != JsonTokenType.Number || !reader.TryGetDecimal(out var value))
        {
            throw new JsonException("ShoppingMall amount must be a JSON number in the Decimal range.");
        }

        return value;
    }

    public override void Write(
        Utf8JsonWriter writer,
        decimal value,
        JsonSerializerOptions options) => writer.WriteNumberValue(value);
}

public sealed class NullableDecimalNumberJsonConverter : JsonConverter<decimal?>
{
    public override decimal? Read(
        ref Utf8JsonReader reader,
        Type typeToConvert,
        JsonSerializerOptions options)
    {
        if (reader.TokenType == JsonTokenType.Null)
        {
            return null;
        }

        if (reader.TokenType != JsonTokenType.Number || !reader.TryGetDecimal(out var value))
        {
            throw new JsonException("ShoppingMall amount must be null or a JSON number in the Decimal range.");
        }

        return value;
    }

    public override void Write(
        Utf8JsonWriter writer,
        decimal? value,
        JsonSerializerOptions options)
    {
        if (value is null)
        {
            writer.WriteNullValue();
            return;
        }

        writer.WriteNumberValue(value.Value);
    }
}
