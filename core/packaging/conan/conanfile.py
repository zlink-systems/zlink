from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, cmake_layout, CMakeToolchain, CMakeDeps
from conan.tools.files import collect_libs, get, copy
import os


required_conan_version = ">=2.1"

# Core release archives, by the (os, arch) they were built for. Everything else
# builds from source.
PREBUILT_PLATFORMS = {
    ("Linux", "x86_64"): "linux-x64",
    ("Linux", "armv8"): "linux-arm64",
    ("Macos", "armv8"): "macos-arm64",
    ("Windows", "x86_64"): "windows-x64",
}


def _enabled(options, name):
    """True when an option is set and on. Option values read back as strings."""
    value = options.get_safe(name)
    return value is not None and str(value) in ("True", "1")


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

    def set_version(self):
        # The repository's VERSION file owns the Core version, so `conan create
        # core/packaging/conan` needs no --version. An export from outside the
        # repository has no VERSION file and must pass one.
        if self.version:
            return
        version_file = os.path.join(self.recipe_folder, "..", "..", "..", "VERSION")
        if os.path.isfile(version_file):
            with open(version_file, encoding="utf-8") as handle:
                for line in handle:
                    key, _, value = line.partition("=")
                    if key.strip() == "LIBZLINK_VERSION":
                        self.version = value.strip()
                        return
        raise ConanInvalidConfiguration(
            "zlink takes its version from LIBZLINK_VERSION in the repository's "
            "VERSION file. Exported without it, pass --version=<x.y.z>."
        )

    def _prebuilt_platform(self, settings=None, options=None):
        """The release archive to install, or None to build from source.

        Only the shared library is taken prebuilt out of the release this
        recipe pins. Its version script exports the zlink_* entry points and
        nothing else, so the Boost that Core was built with cannot reach the
        consumer's link. The libzlink.a in the 1.2.0 archive has no such
        filter -- it carries the default-visible boost::* symbols of Core's own
        Boost tree, which collapse onto the consumer's Boost at link time and
        crash at run time. A static Core is therefore built here, against the
        Boost this build resolves.

        Core 1.3.0 gives libzlink.a the same public surface as the shared
        library on Linux and macOS (issue #418), so a later recipe pinned to
        1.3.0 or newer can take the archive for those two as well. Windows
        cannot: MSVC has no way to localize a symbol in a static .lib.
        """
        # package_id() may not read self.settings/self.options, so it passes
        # the ones on self.info instead.
        settings = self.settings if settings is None else settings
        options = self.options if options is None else options
        if not _enabled(options, "shared"):
            return None
        platform = PREBUILT_PLATFORMS.get((str(settings.os), str(settings.arch)))
        if platform is None:
            return None
        # The published archive is always built WITH_TLS=ON.
        if not _enabled(options, "with_tls"):
            return None
        return self.conan_data.get("binaries", {}).get(str(self.version), {}).get(platform)

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

    def package_id(self):
        if self._prebuilt_platform(self.info.settings, self.info.options):
            # A release build of a pure C API: core/include/zlink.h puts no C++
            # and no Boost type on the boundary, so one binary serves every
            # compiler and C++ standard. The archive is also a Release build
            # only -- there is no debug Core to hand a Debug consumer. os, arch
            # and the options stay: they select which files are packaged.
            del self.info.settings.compiler
            del self.info.settings.build_type

    def validate(self):
        if self._prebuilt_platform():
            return
        if self.settings.compiler.get_safe("cppstd"):
            try:
                check_min_cppstd(self, "17")
            except ConanInvalidConfiguration as exc:
                raise ConanInvalidConfiguration(
                    "zlink requires C++17 or later"
                ) from exc

    def source(self):
        # source() runs once per recipe revision, before any configuration is
        # known, so it always fetches the source tree even when this build will
        # install a release archive instead. The archive is per-configuration
        # and is fetched in build().
        src = self.conan_data["sources"][self.version]
        get(self, **src, strip_root=False)

    def generate(self):
        if self._prebuilt_platform():
            return
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
        archive = self._prebuilt_platform()
        if archive:
            self.output.info(
                f"Installing the prebuilt Core {self.version} archive for "
                f"{self.settings.os}/{self.settings.arch}."
            )
            get(self, **archive, strip_root=True, destination=self._prebuilt_folder)
            return
        self.output.info(
            f"No prebuilt Core {self.version} archive for {self.settings.os}/"
            f"{self.settings.arch} with shared={self.options.shared} and "
            f"with_tls={self.options.with_tls}; building from source."
        )
        cmake = CMake(self)
        cmake.configure(build_script_folder=os.path.join(self.source_folder, "core"))
        cmake.build()

    @property
    def _prebuilt_folder(self):
        return os.path.join(self.build_folder, "prebuilt")

    def _package_prebuilt(self):
        prefix = self._prebuilt_folder
        copy(self, "*", src=os.path.join(prefix, "include"),
             dst=os.path.join(self.package_folder, "include"))
        # The CMake package that ships in the archive is not installed:
        # CMakeDeps writes the config Conan consumers use, and package_info()
        # below owns its contents.
        copy(self, "zlink.dll", src=os.path.join(prefix, "bin"),
             dst=os.path.join(self.package_folder, "bin"))
        found = []
        for pattern in ("libzlink.so*", "libzlink*.dylib", "zlink.lib"):
            found += copy(self, pattern, src=os.path.join(prefix, "lib"),
                          dst=os.path.join(self.package_folder, "lib"))
        if not found:
            raise ConanInvalidConfiguration(
                f"The Core {self.version} archive for {self.settings.os}/"
                f"{self.settings.arch} has no shared library."
            )
        copy(self, "LICENSE.txt", src=os.path.join(prefix, "share", "zlink"),
             dst=os.path.join(self.package_folder, "licenses"))

    def package(self):
        if self._prebuilt_platform():
            self._package_prebuilt()
            return
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
