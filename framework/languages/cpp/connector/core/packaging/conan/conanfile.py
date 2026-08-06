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
            "framework/languages/cpp/connector",
            "framework/languages/cpp/extensions",
            "framework/languages/cpp/framework",
            "framework/languages/cpp/http-client",
            "bindings/cpp/cmake",
            "bindings/cpp/include",
            "bindings/cpp/native",
            "bindings/cpp/src",
            "core/external/boost",
            "core/include",
        ]
        copy(
            self,
            "CMakeLists.txt",
            src=os.path.join(repo_root, "framework/languages/cpp"),
            dst=os.path.join(self.export_sources_folder, "framework/languages/cpp"),
        )
        copy(
            self,
            "CMakeLists.txt",
            src=os.path.join(repo_root, "bindings/cpp"),
            dst=os.path.join(self.export_sources_folder, "bindings/cpp"),
        )
        copy(self, "LICENSE", src=repo_root, dst=self.export_sources_folder)
        for folder in folders:
            copy(
                self,
                "*",
                src=os.path.join(repo_root, folder),
                dst=os.path.join(self.export_sources_folder, folder),
            )

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["ZLINK_FRAMEWORK_CPP_BUILD_TESTS"] = False
        tc.variables["ZLINK_FRAMEWORK_CPP_BUILD_SAMPLES"] = False
        tc.variables["ZLINK_FRAMEWORK_CPP_INSTALL_FRAMEWORK"] = False
        tc.variables["ZLINK_STREAM_CONNECTOR_BUILD_E2E_CLIENT"] = False
        tc.variables["ZLINK_STREAM_CONNECTOR_BUILD_UNREAL"] = False
        tc.variables["ZLINK_STREAM_CONNECTOR_BUILD_GODOT"] = False
        tc.variables["ZLINK_STREAM_CONNECTOR_BUILD_AXMOL"] = False
        tc.variables["ZLINK_STREAM_CONNECTOR_WITH_LZ4"] = bool(self.options.with_lz4)
        tc.variables["ZLINK_STREAM_CONNECTOR_WITH_TLS"] = bool(self.options.with_tls)
        tc.variables["ZLINK_STREAM_CONNECTOR_WITH_WEBSOCKET"] = bool(self.options.with_websocket)
        tc.variables["BUILD_SHARED_LIBS"] = bool(self.options.shared)
        tc.generate()
        CMakeDeps(self).generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure(build_script_folder="framework/languages/cpp")
        cmake.build(target="zlink_stream_connector")

    def package(self):
        CMake(self).install()

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "zlink_stream_connector_cpp")
        self.cpp_info.set_property("cmake_target_name", "zlink::stream_connector")
        self.cpp_info.libs = ["zlink_stream_connector"]
