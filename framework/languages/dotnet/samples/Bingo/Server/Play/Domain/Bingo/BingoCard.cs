namespace Bingo.Server.Play.Domain.Bingo;

internal sealed class BingoCard
{
    public const int Size = 3;
    public const int CellCount = Size * Size;
    public const int FreeCellIndex = 4;
    private readonly bool[] _marks;

    private readonly int[] _numbers;

    private BingoCard(int[] numbers, bool[] marks)
    {
        _numbers = numbers;
        _marks = marks;
    }

    public int CompletedLines => CountCompletedLines();

    public static BingoCard FromSubmittedNumbers(IReadOnlyList<int> submitted)
    {
        if (submitted.Count != CellCount)
            throw new InvalidOperationException($"Bingo card must contain {CellCount} cells.");

        var numbers = submitted.ToArray();
        if (numbers[FreeCellIndex] != 0) throw new InvalidOperationException("Bingo card center cell must be 0.");

        var playableNumbers = numbers
            .Where(static number => number != 0)
            .ToArray();
        if (playableNumbers.Any(static number => number < 1 || number > 15))
            throw new InvalidOperationException("Bingo card numbers must be between 1 and 15.");

        if (playableNumbers.Distinct().Count() != playableNumbers.Length)
            throw new InvalidOperationException("Bingo card numbers must be unique.");

        var marks = new bool[CellCount];
        marks[FreeCellIndex] = true;
        return new BingoCard(numbers, marks);
    }

    public int[] NumbersSnapshot()
    {
        return (int[])_numbers.Clone();
    }

    public bool[] MarksSnapshot()
    {
        return (bool[])_marks.Clone();
    }

    public BingoCardSnapshot Snapshot()
    {
        return new BingoCardSnapshot(NumbersSnapshot(), MarksSnapshot(), CompletedLines);
    }

    public void MarkDrawnNumber(int number)
    {
        for (var i = 0; i < _numbers.Length; i++)
            if (_numbers[i] == number)
                _marks[i] = true;
    }

    private int CountCompletedLines()
    {
        var completed = 0;
        for (var row = 0; row < Size; row++) completed += IsRowComplete(row) ? 1 : 0;

        for (var column = 0; column < Size; column++) completed += IsColumnComplete(column) ? 1 : 0;

        completed += IsDiagonalComplete(0, Size + 1) ? 1 : 0;
        completed += IsDiagonalComplete(Size - 1, Size - 1) ? 1 : 0;
        return completed;
    }

    private bool IsRowComplete(int row)
    {
        var start = row * Size;
        for (var i = 0; i < Size; i++)
            if (!_marks[start + i])
                return false;

        return true;
    }

    private bool IsColumnComplete(int column)
    {
        for (var row = 0; row < Size; row++)
            if (!_marks[row * Size + column])
                return false;

        return true;
    }

    private bool IsDiagonalComplete(int start, int step)
    {
        for (var i = 0; i < Size; i++)
            if (!_marks[start + i * step])
                return false;

        return true;
    }
}