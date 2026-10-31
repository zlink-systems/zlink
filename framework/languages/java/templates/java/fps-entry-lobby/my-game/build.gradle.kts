extra["frameworkVersion"] = "0.31.0"
extra["bindingVersion"] = "1.21.0"
subprojects {
    apply(plugin = "java-library")
    extensions.configure<JavaPluginExtension> { toolchain.languageVersion.set(JavaLanguageVersion.of(25)) }
    dependencies {
        "api"("systems.zlink:zlink-framework-core:${rootProject.extra["frameworkVersion"]}")
        "api"("systems.zlink:zlink:${rootProject.extra["bindingVersion"]}")
        "api"("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    }
}
