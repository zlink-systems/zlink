package systems.zlink.samples.kotlin.zoneworld.client

suspend fun main(args: Array<String>) {
    val options = ClientOptions.load(args)
    val selected = if (options.scenarios.equals("all", ignoreCase = true)) {
        Scenarios.clientDriven.keys
    } else {
        options.scenarios.split(',').toCollection(linkedSetOf())
    }
    selected.forEach { id ->
        val scenario = Scenarios.clientDriven[id] ?: Scenarios.runnerDriven[id]
            ?: error("unknown scenario: $id")
        scenario(options)
        println("scenario $id passed")
    }
}
