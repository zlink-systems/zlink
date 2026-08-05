package systems.zlink.samples.bingo.server.play.domain.bingo;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;

public final class BingoCard {
    public static final int Size = 3;
    public static final int CellCount = Size * Size;
    public static final int FreeCellIndex = 4;

    private final List<Integer> numbers;
    private final List<Boolean> marks;

    private BingoCard(List<Integer> numbers, List<Boolean> marks) {
        this.numbers = numbers;
        this.marks = marks;
    }

    public static BingoCard from(List<Integer> submitted) {
        if (submitted == null || submitted.size() != CellCount) {
            throw new IllegalArgumentException("bingo card must contain 9 cells");
        }
        HashSet<Integer> seen = new HashSet<>();
        ArrayList<Integer> numbers = new ArrayList<>(submitted);
        for (int i = 0; i < numbers.size(); i++) {
            int number = numbers.get(i);
            if (i == FreeCellIndex) {
                if (number != 0) {
                    throw new IllegalArgumentException("bingo card center cell must be 0");
                }
                continue;
            }
            if (number < 1 || number > 15 || !seen.add(number)) {
                throw new IllegalArgumentException("bingo card numbers must be unique values from 1 to 15");
            }
        }
        ArrayList<Boolean> marks = new ArrayList<>();
        for (int i = 0; i < CellCount; i++) {
            marks.add(i == FreeCellIndex);
        }
        return new BingoCard(numbers, marks);
    }

    static BingoCard restore(List<Integer> numbers, List<Boolean> marks) {
        BingoCard card = from(numbers);
        if (marks == null || marks.size() != CellCount) {
            throw new IllegalArgumentException("bingo card marks must contain 9 cells");
        }
        return new BingoCard(new ArrayList<>(numbers), new ArrayList<>(marks));
    }

    public void markDrawnNumber(int number) {
        for (int i = 0; i < numbers.size(); i++) {
            if (numbers.get(i) == number) {
                marks.set(i, true);
            }
        }
    }

    public int completedLines() {
        int completed = 0;
        for (int row = 0; row < Size; row++) {
            completed += isRowComplete(row) ? 1 : 0;
        }
        for (int column = 0; column < Size; column++) {
            completed += isColumnComplete(column) ? 1 : 0;
        }
        completed += isDiagonalComplete(0, Size + 1) ? 1 : 0;
        completed += isDiagonalComplete(Size - 1, Size - 1) ? 1 : 0;
        return completed;
    }

    public List<Integer> numbersSnapshot() {
        return List.copyOf(numbers);
    }

    public List<Boolean> marksSnapshot() {
        return List.copyOf(marks);
    }

    private boolean isRowComplete(int row) {
        int start = row * Size;
        for (int i = 0; i < Size; i++) {
            if (!marks.get(start + i)) {
                return false;
            }
        }
        return true;
    }

    private boolean isColumnComplete(int column) {
        for (int row = 0; row < Size; row++) {
            if (!marks.get((row * Size) + column)) {
                return false;
            }
        }
        return true;
    }

    private boolean isDiagonalComplete(int start, int step) {
        for (int i = 0; i < Size; i++) {
            if (!marks.get(start + (i * step))) {
                return false;
            }
        }
        return true;
    }
}
