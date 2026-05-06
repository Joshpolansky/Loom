from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout


class MyModuleConan(ConanFile):
    """
    Template conanfile for a Loom module plugin.

    Copy this file into your module project and adjust:
      - `name`       — your module's package name
      - `version`    — your module's version
      - `requires`   — pin loom to the version you're building against
    """
    name = "my-module"
    version = "0.1.0"
    settings = "os", "compiler", "build_type", "arch"
    exports_sources = "*.cpp", "*.hpp", "CMakeLists.txt"

    def requirements(self):
        # Pin to the loom SDK version you're targeting.
        self.requires("loom/0.1.0@local/stable")
        # Pull in the runtime library so you can build and debug the runtime
        # executable directly from your project as a native CMake target.
        self.requires("loom-runtime/0.1.0@local/stable")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        # Module plugins are loaded at runtime by the Loom runtime binary.
        # Install to lib/ so consumers can locate the built plugin.
        # Suffix is .dll on Windows, .so on Linux/macOS (MODULE libraries use .so on macOS too).
        from conan.tools.files import copy
        import os
        suffixes = {"Windows": "*.dll", "Linux": "*.so", "Macos": "*.so"}
        pattern = suffixes.get(str(self.settings.os), "*.so")
        copy(self, pattern,
             src=os.path.join(self.build_folder, "modules"),
             dst=os.path.join(self.package_folder, "lib"),
             keep_path=False)

    def package_info(self):
        self.cpp_info.libdirs = ["lib"]
