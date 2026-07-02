# Smoke test for loom_add_module() itself (native + wasm): every other bundled
# module still hand-rolls add_library(MODULE) + the same boilerplate this
# helper now replaces. If this diverges from the others in any observable way,
# something is wrong with the helper, not with this module.
loom_add_module(example_motor
    SOURCES motor_module.cpp
    INCLUDE ${LOOM_MODULES_DIR}
)

