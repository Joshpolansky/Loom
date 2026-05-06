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
        # SDK deps — glaze listed explicitly since the SDK is consumed as source here
        self.requires("glaze/7.2.0")
        # Runtime deps
        self.requires("spdlog/1.17.0")
        self.requires("crowcpp-crow/1.3.0")
        if self.options.with_tests:
            self.requires("gtest/1.15.0")
