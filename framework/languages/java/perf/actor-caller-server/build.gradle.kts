plugins {
    application
}

dependencies {
    implementation(project(":server-support"))
}

application {
    applicationName = "actor-caller-server"
    mainClass.set("systems.zlink.framework.perf.servers.actorcaller.Program")
}
