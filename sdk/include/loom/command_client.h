#pragma once

#include <memory>
#include <string>
#include <utility>

#include "command.h"
#include "module.h"

// ============================================================================
// Consumer side of the async-command primitive: CommandClient (submit) and
// CommandFb (the reusable Execute-edge / status-mirror base for PLCopen-style
// function blocks). Domain layers build thin FBs on top of these.
//
// These need a module handle (for the Bus lookup + runtime heap), so they live
// here rather than in command.h.
// ============================================================================

namespace loom {

/// Handle a consumer module uses to submit commands to a named provider.
/// Resolves the provider's channel on the Bus fresh on each submit (hot-reload
/// safe), allocates the status cell from the runtime heap, and enqueues it.
class CommandClient {
public:
    CommandClient(IModule* self, std::string provider)
        : self_(self), provider_(std::move(provider)) {}

    /// Submit to sub-entity `target` (0 for single-entity providers). Returns
    /// the status cell to hold, or nullptr if the provider is unreachable or
    /// the heap is exhausted.
    std::shared_ptr<CommandStatus> submit(std::string command, uint32_t target,
                                          std::string params, BufferMode bm) {
        if (!self_ || !self_->bus()) return nullptr;
        auto* ch = self_->bus()->commandChannel(provider_);
        if (!ch) return nullptr;
        auto status = self_->makeShared<CommandStatus>();
        if (!status) return nullptr;
        ch->submit(CommandSubmission{std::move(command), target, std::move(params), bm, status});
        return status;
    }

    std::shared_ptr<CommandStatus> submit(std::string command, std::string params, BufferMode bm) {
        return submit(std::move(command), 0, std::move(params), bm);
    }

    const std::string& provider() const { return provider_; }

private:
    IModule*    self_;
    std::string provider_;
};

} // namespace loom
