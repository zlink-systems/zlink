plugins {
    application
}

dependencies {
    implementation(project(":server-support"))
}

application {
    applicationName = "subscriber-server"
    mainClass.set("systems.zlink.framework.perf.servers.subscriber.Program")
}
