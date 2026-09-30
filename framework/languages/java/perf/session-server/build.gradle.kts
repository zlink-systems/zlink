plugins {
    application
}

dependencies {
    implementation(project(":server-support"))
}

application {
    applicationName = "session-server"
    mainClass.set("systems.zlink.framework.perf.servers.session.Program")
}
