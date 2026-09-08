from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, cmake_layout, CMakeToolchain, CMakeDeps
from conan.tools.files import collect_libs, get, copy
import os


required_conan_version = ">=2.1"


class ZlinkConan(ConanFile):
    name = "zlink"
    license = "MPL-2.0"
    url = "https://github.com/conan-io/conan-center-index"
    homepage = "https://github.com/zlink-systems/zlink"
    description = "High-performance asynchronous messaging library"
    topics = ("messaging", "networking", "ipc", "asynchronous")

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

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def layout(self):
        cmake_layout(self)

    def requirements(self):
        if self.options.with_tls:
            self.requires("openssl/[>=3.0 <4]")

    def validate(self):
        if self.settings.compiler.get_safe("cppstd"):
            try:
                check_min_cppstd(self, "17")
            except ConanInvalidConfiguration as exc:
                raise ConanInvalidConfiguration(
                    "zlink requires C++17 or later"
                ) from exc

    def source(self):
        src = self.conan_data["sources"][self.version]
        get(self, **src, strip_root=False)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["BUILD_SHARED"] = bool(self.options.shared)
        tc.variables["BUILD_STATIC"] = not bool(self.options.shared)
        tc.variables["BUILD_TESTS"] = False
        tc.variables["BUILD_BENCHMARKS"] = False
        tc.variables["WITH_DOC"] = False
        tc.variables["ENABLE_CPACK"] = False
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
        cmake_target = "libzlink" if self.options.shared else "libzlink-static"
        self.cpp_info.set_property("cmake_file_name", "zlink")
        self.cpp_info.set_property("cmake_target_name", cmake_target)
        self.cpp_info.set_property("pkg_config_name", "libzlink")
        self.cpp_info.libs = collect_libs(self)
        if not self.options.shared:
            self.cpp_info.defines.append("ZLINK_STATIC")
        if self.options.with_tls:
            self.cpp_info.requires.append("openssl::openssl")
        if self.settings.os in ("Linux", "FreeBSD"):
            self.cpp_info.system_libs.extend(["pthread", "rt"])
