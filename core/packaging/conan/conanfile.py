from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout, CMakeToolchain, CMakeDeps
from conan.tools.files import get, copy
import os


class ZlinkConan(ConanFile):
    name = "zlink"
    license = "MPL-2.0"
    url = "https://github.com/ulala-x/zlink"
    description = "libzlink core library"
    topics = ("messaging", "networking", "ipc", "asio")

    settings = "os", "arch", "compiler", "build_type"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_tls": [True, False],
    }
    default_options = {
        "shared": True,
        "fPIC": True,
        "with_tls": True,
    }

    package_type = "library"

    def layout(self):
        cmake_layout(self)

    def source(self):
        src = self.conan_data["sources"][self.version]
        get(self, **src, strip_root=True)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["BUILD_SHARED"] = bool(self.options.shared)
        tc.variables["BUILD_STATIC"] = not bool(self.options.shared)
        tc.variables["BUILD_TESTS"] = False
        tc.variables["BUILD_BENCHMARKS"] = False
        tc.variables["WITH_TLS"] = bool(self.options.with_tls)
        tc.variables["ZLINK_CXX_STANDARD"] = "17"
        tc.generate()
        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure(build_script_folder=os.path.join(self.source_folder, "core"))
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(self, "LICENSE", src=self.source_folder, dst=os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        self.cpp_info.libs = ["zlink"]
        if self.settings.os in ("Linux", "FreeBSD"):
            self.cpp_info.system_libs.extend(["pthread"])
