package systems.zlink.e2e.kotlin.automaticturn

fun main(args: Array<String>) {
    Env.configure(args)
    ClientApplication.run(*args.drop(2).toTypedArray())
}
