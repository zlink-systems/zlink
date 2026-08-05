package systems.zlink.samples.kotlin.bingo.server.play.domain.bingo

class BingoCard private constructor(
    private val numbers: MutableList<Int>,
    private val marks: MutableList<Boolean>,
) {
    fun markDrawnNumber(number: Int) {
        numbers.forEachIndexed { index, value ->
            if (value == number) {
                marks[index] = true
            }
        }
    }

    fun completedLines(): Int {
        var completed = 0
        for (row in 0 until Size) {
            if (isRowComplete(row)) {
                completed++
            }
        }
        for (column in 0 until Size) {
            if (isColumnComplete(column)) {
                completed++
            }
        }
        if (isDiagonalComplete(0, Size + 1)) {
            completed++
        }
        if (isDiagonalComplete(Size - 1, Size - 1)) {
            completed++
        }
        return completed
    }

    fun numbersSnapshot(): List<Int> = numbers.toList()

    fun marksSnapshot(): List<Boolean> = marks.toList()

    private fun isRowComplete(row: Int): Boolean {
        val start = row * Size
        return (0 until Size).all { marks[start + it] }
    }

    private fun isColumnComplete(column: Int): Boolean =
        (0 until Size).all { row -> marks[(row * Size) + column] }

    private fun isDiagonalComplete(start: Int, step: Int): Boolean =
        (0 until Size).all { marks[start + (it * step)] }

    companion object {
        private const val Size = 3
        private const val CellCount = Size * Size
        private const val FreeCellIndex = 4

        fun from(values: List<Int>): BingoCard {
            require(values.size == CellCount) { "Bingo card must contain $CellCount cells." }
            require(values[FreeCellIndex] == 0) { "Bingo card center cell must be the free cell." }
            val seen = mutableSetOf<Int>()
            values.forEachIndexed { index, value ->
                if (index == FreeCellIndex) {
                    return@forEachIndexed
                }
                require(value in 1..15) { "Bingo card value is outside the sample draw range: $value" }
                require(seen.add(value)) { "Bingo card contains duplicate value: $value" }
            }
            val numbers = values.toMutableList()
            val marks = MutableList(CellCount) { it == FreeCellIndex }
            return BingoCard(numbers, marks)
        }

        fun restore(values: List<Int>, marks: List<Boolean>): BingoCard {
            from(values)
            require(marks.size == CellCount) { "Bingo card marks must contain $CellCount cells." }
            return BingoCard(values.toMutableList(), marks.toMutableList())
        }
    }
}
