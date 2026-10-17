package MyGame.channel.domain

object GoldBalance {
    private const val MAX_GOLD = 9_007_199_254_740_991L

    fun validate(gold: Long): Long {
        require(gold in 0..MAX_GOLD) { "Invalid gold" }
        return gold
    }

    fun earn(gold: Long, amount: Long): Long {
        require(amount > 0 && amount <= MAX_GOLD - gold) { "Invalid earning amount" }
        return gold + amount
    }
}
