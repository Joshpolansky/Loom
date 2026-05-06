#include <loom/module.h>

// Verify that the SDK headers are consumable and the loom:: types are accessible.
static_assert(loom::kApiVersion > 0, "loom::kApiVersion must be defined");

int main() {
    return 0;
}
