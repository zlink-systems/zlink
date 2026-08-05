namespace Bingo.Server.Play.Domain.Bingo;

internal sealed class BingoGame(BingoRoomSettings settings, IReadOnlyList<string> participantActorIds)
{
    private readonly Dictionary<string, BingoCard> _cards = new(StringComparer.Ordinal);
    private readonly Queue<int> _drawDeck = new(Enumerable.Range(1, settings.MaxDrawNumber));
    private readonly List<int> _drawnNumbers = [];
    private readonly HashSet<string> _participants = participantActorIds.ToHashSet(StringComparer.Ordinal);
    private readonly List<string> _winners = [];

    public bool IsFinished { get; private set; }

    public bool IsReadyToDraw => !IsFinished
                                 && _participants.Count > 0
                                 && _cards.Count == _participants.Count;

    public BingoGameSnapshot Snapshot()
    {
        return new BingoGameSnapshot(
            _drawnNumbers.Count,
            _drawnNumbers.Count == 0 ? null : _drawnNumbers[^1],
            _drawnNumbers.ToArray(),
            _winners.ToArray());
    }

    public BingoCardSnapshot CardSnapshot(string actorId)
    {
        return _cards.TryGetValue(actorId, out var card)
            ? card.Snapshot()
            : BingoCardSnapshot.Empty;
    }

    public void SubmitCard(string actorId, BingoCard card)
    {
        if (IsFinished) throw new InvalidOperationException("Bingo game has already finished.");

        if (!_participants.Contains(actorId))
            throw new InvalidOperationException("Player is not part of this bingo game.");

        if (_cards.ContainsKey(actorId)) throw new InvalidOperationException("Bingo card has already been submitted.");

        _cards.Add(actorId, card);
    }

    public BingoDrawResult DrawNextNumber()
    {
        if (IsFinished) return new BingoDrawResult(null, true);

        if (!IsReadyToDraw) return new BingoDrawResult(null, false);

        if (_drawDeck.Count == 0)
        {
            IsFinished = true;
            return new BingoDrawResult(null, true);
        }

        var number = _drawDeck.Dequeue();
        _drawnNumbers.Add(number);

        foreach (var (actorId, card) in _cards)
        {
            var before = card.CompletedLines;
            card.MarkDrawnNumber(number);
            if (card.CompletedLines > before) _winners.Add(actorId);
        }

        if (_winners.Count > 0 || _drawDeck.Count == 0) IsFinished = true;

        return new BingoDrawResult(number, IsFinished);
    }
}

internal sealed record BingoDrawResult(
    int? Number,
    bool IsFinished);

internal sealed record BingoGameSnapshot(
    int DrawSeq,
    int? LastDrawnNumber,
    IReadOnlyList<int> DrawnNumbers,
    IReadOnlyList<string> Winners);

internal sealed record BingoCardSnapshot(
    int[] Numbers,
    bool[] Marks,
    int CompletedLines)
{
    public static BingoCardSnapshot Empty { get; } = new([], [], 0);
}