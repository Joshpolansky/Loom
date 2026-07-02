from conan import ConanFile
import os


class LoomDevConan(ConanFile):
    """
    Dev-only consumer conanfile. Used by 'just setup' to install all
    dependencies for the full repo build (sdk + runtime + modules + tests).
    This file is NOT used for packaging — see sdk/conanfile.py and
    runtime/conanfile.py for the distributable Conan packages.
    """
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    options = {"with_tests": [True, False]}
    default_options = {"with_tests": True}

    def requirements(self):
        # Shared by every target. glaze is listed explicitly since the SDK is
        # consumed as source here; spdlog is header-only on the WASM build (set
        # in conan/profiles/emscripten) so no library is cross-compiled.
        self.requires("glaze/7.2.0")
        self.requires("spdlog/1.17.0")

        # Native-host-only deps. The WASM runtime is thread-free and server-less:
        # no HTTP/WebSocket (Crow), no in-process stack symbolization (cpptrace),
        # and tests don't run there.
        if self.settings.os != "Emscripten":
            self.requires("crowcpp-crow/1.3.0")
            self.requires("cpptrace/0.8.3")
            if self.options.with_tests:
                self.requires("gtest/1.15.0")
