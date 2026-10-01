plugins {
    application
}

dependencies {
    implementation(project(":server-support"))
}

application {
    applicationName = "actor-server"
    mainClass.set("systems.zlink.framework.perf.servers.actor.Program")
}
