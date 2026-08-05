package systems.zlink.e2e.kotlin.resiliencelifecycle

fun main(args: Array<String>) {
    ConsumerApplication.run(*args).use { _ ->
        Thread.currentThread().join()
    }
}
