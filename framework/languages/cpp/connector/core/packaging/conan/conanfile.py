from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy
import os


class ZlinkStreamConnectorConan(ConanFile):
    name = "zlink-stream-connector"
    version = "0.10.0"
    license = "MPL-2.0"
    url = "https://github.com/zlink-systems/zlink"
    description = "ZLink C++ Stream Connector core library"
    package_type = "library"
    settings = "os", "arch", "compiler", "build_type"
    options = {
        "with_lz4": [True, False],
        "with_tls": [True, False],
        "with_websocket": [True, False],
        "shared": [True, False],
    }
    default_options = {
        "with_lz4": True,
        "with_tls": True,
        "with_websocket": True,
        "shared": False,
    }

    def layout(self):
        cmake_layout(self)

    def export_sources(self):
        repo_root = os.path.abspath(
            os.path.join(self.recipe_folder, "../../../../../../../")
        )
        folders = [
            "framework/languages/cpp/cmake",
            "framework/languages/cpp/common",
            "framework/languages/cpp/connector",
        ]
        copy(
            self,
            "VERSION",
            src=os.path.join(repo_root, "framework/languages/cpp"),
            dst=os.path.join(self.export_sources_folder, "framework/languages/cpp"),
        )
        copy(self, "LICENSE", src=repo_root, dst=self.export_sources_folder)
        copy(
            self,
            "json_stream_connector.hpp",
            src=os.path.join(repo_root, "framework/languages/cpp/framework/include/zlink/framework/codecs"),
            dst=os.path.join(
                self.export_sources_folder,
                "framework/languages/cpp/framework/include/zlink/framework/codecs",
            ),
        )
        for folder in folders:
            copy(
                self,
                "*",
                src=os.path.join(repo_root, folder),
                dst=os.path.join(self.export_sources_folder, folder),
            )

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["ZLINK_FRAMEWORK_CPP_USE_SYSTEM_BOOST"] = True
        tc.variables["ZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT"] = False
        tc.variables["ZLINK_STREAM_CONNECTOR_BUILD_UNREAL"] = False
        tc.variables["ZLINK_STREAM_CONNECTOR_BUILD_GODOT"] = False
        tc.variables["ZLINK_STREAM_CONNECTOR_BUILD_AXMOL"] = False
        tc.variables["ZLINK_STREAM_CONNECTOR_WITH_LZ4"] = bool(self.options.with_lz4)
        tc.variables["ZLINK_STREAM_CONNECTOR_WITH_TLS"] = bool(self.options.with_tls)
        tc.variables["ZLINK_STREAM_CONNECTOR_WITH_WEBSOCKET"] = bool(self.options.with_websocket)
        tc.variables["ZLINK_FRAMEWORK_CPP_SHARED"] = bool(self.options.shared)
        tc.generate()
        CMakeDeps(self).generate()

    def requirements(self):
        self.requires("boost/1.85.0")
        self.requires("nlohmann_json/3.11.3")
        if self.options.with_tls:
            self.requires("openssl/[>=3.0 <4]")
        if self.options.with_lz4:
            self.requires("lz4/1.9.4")

    def build(self):
        cmake = CMake(self)
        cmake.configure(build_script_folder="framework/languages/cpp/connector")
        cmake.build(target="zlink_stream_connector")

    def package(self):
        CMake(self).install(component="StreamConnector")

    def package_info(self):
        self.cpp_info.set_property("cmake_find_mode", "none")
        self.cpp_info.builddirs.append("lib/cmake/zlink_stream_connector_cpp")
        self.cpp_info.libs = ["zlink_stream_connector"]
