extra["junitVersion"] = "5.11.4"
extra["frameworkVersion"] = "0.30.2"
extra["bindingVersion"] = "1.20.0"
extra["springBootVersion"] = "3.5.14"
subprojects {
    apply(plugin = "java-library")
    extensions.configure<JavaPluginExtension> { toolchain.languageVersion.set(JavaLanguageVersion.of(25)) }
    dependencies {
        "api"("systems.zlink:zlink-framework-core:${rootProject.extra["frameworkVersion"]}")
        "api"("systems.zlink:zlink:${rootProject.extra["bindingVersion"]}")
        "api"("com.fasterxml.jackson.core:jackson-databind:2.17.2")
    }
}
