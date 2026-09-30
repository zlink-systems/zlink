plugins {
    application
}

dependencies {
    implementation(project(":server-support"))
}

application {
    applicationName = "channel-server"
    mainClass.set("systems.zlink.framework.perf.servers.channel.Program")
}
