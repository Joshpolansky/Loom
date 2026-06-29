#include <loom/module.h>
#include <loom/export.h>
#include <loom/command.h>

#include <cstdint>
#include <vector>

// ============================================================================
// CommandProbe — a minimal async-command *provider* used by integration tests.
//
// In init() it registers a CommandChannel on the bus (provideCommands). Each
// cyclic() it drains the channel and marks every submitted command Done by
// writing through the command's weak_ptr<CommandStatus>. Exercises the generic
// command primitive end to end across a real loaded/unloaded module boundary.
// ============================================================================

struct CPConfig  { int _unused = 0; };
struct CPRecipe  { int _unused = 0; };
struct CPRuntime {
    uint64_t processed     = 0;
    uint64_t last_command  = 0;
    uint32_t last_target   = 0;
};

class CommandProbe : public loom::Module<CPConfig, CPRecipe, CPRuntime> {
public:
    LOOM_MODULE_HEADER("CommandProbe", "1.0.0")

    void init(const loom::InitContext&) override {
        provideCommands(channel_);
    }

    void cyclic() override {
        channel_.drain(drained_);
        for (auto& s : drained_) {
            runtime_.processed++;
            runtime_.last_command = s.command;
            runtime_.last_target  = s.target;
            if (auto st = s.status.lock()) {   // write status THROUGH the weak ref
                st->phase.store(loom::CmdPhase::Done);
                st->done.store(true);
                st->progress.store(1.0);
            }
        }
    }

    void exit() override {}

private:
    loom::CommandChannel                 channel_;
    std::vector<loom::CommandSubmission> drained_;
};

LOOM_REGISTER_MODULE(CommandProbe)
