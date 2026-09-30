plugins {
    application
}

dependencies {
    implementation(project(":server-support"))
}

application {
    applicationName = "session-actor-local-server"
    mainClass.set("systems.zlink.framework.perf.servers.sessionactorlocal.Program")
}
