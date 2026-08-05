namespace TicTacToe.Server.Play.Domain.TicTacToe;

internal sealed class TicTacToeBoard
{
    private const char Empty = '.';
    private const int CellCount = 9;

    private readonly char[] _cells = Enumerable.Repeat(Empty, CellCount).ToArray();

    public bool IsFull => _cells.All(static cell => cell != Empty);

    public string Snapshot()
    {
        return new string(_cells);
    }

    public void Place(string mark, int cell)
    {
        if ((uint)cell >= _cells.Length)
            throw new ArgumentOutOfRangeException(nameof(cell), "Cell must be between 0 and 8.");

        if (_cells[cell] != Empty) throw new InvalidOperationException($"Cell {cell} is already occupied.");

        _cells[cell] = mark[0];
    }

    public bool HasWon(string mark)
    {
        ReadOnlySpan<int> lines =
        [
            0, 1, 2,
            3, 4, 5,
            6, 7, 8,
            0, 3, 6,
            1, 4, 7,
            2, 5, 8,
            0, 4, 8,
            2, 4, 6
        ];

        var symbol = mark[0];
        for (var i = 0; i < lines.Length; i += 3)
            if (_cells[lines[i]] == symbol
                && _cells[lines[i + 1]] == symbol
                && _cells[lines[i + 2]] == symbol)
                return true;

        return false;
    }
}