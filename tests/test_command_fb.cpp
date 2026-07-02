#include <gtest/gtest.h>

#include "loom/command.h"

#include <memory>
#include <vector>

// ============================================================================
// loom::CommandFb PLCopen output semantics:
//   - exactly one of {busy, done, command_aborted, error} at a time;
//   - dropping Execute does not cancel — stays Busy until the command finishes;
//   - a terminal result is held while Execute is high and is visible for >= 1
//     cycle even if Execute is already low; resets when Execute is low.
// Plus a write-through round trip through a CommandChannel.
// ============================================================================

namespace {

using namespace loom;

/// Test FB: submits the injected `next` status on the Execute rising edge,
/// mimicking a domain FB's update(). Exposes the protected CommandFb plumbing.
struct PokeFb : CommandFb {
    std::shared_ptr<CommandStatus> next;   // "allocated" status to submit on rising edge
    void tick() {
        const bool r = rising();
        commit(r, r ? next : nullptr);
    }
    void update() override { tick(); }     // satisfy IFunctionBlock
};

/// Count of the mutually-exclusive busy/terminal outputs (active is separate).
int hot(const CommandFb& f) {
    return int(f.busy) + int(f.done) + int(f.command_aborted) + int(f.error);
}

// --- 1.1: exactly one of busy/done/aborted/error at each phase ---------------
TEST(CommandFb, ExactlyOneOutputPerPhase) {
    PokeFb fb;
    fb.next = std::make_shared<CommandStatus>();

    fb.execute = true; fb.tick();                      // accepted
    EXPECT_TRUE(fb.busy);
    EXPECT_EQ(hot(fb), 1);

    fb.next->phase.store(CmdPhase::Active); fb.tick();  // active (busy + active)
    EXPECT_TRUE(fb.busy);
    EXPECT_TRUE(fb.active);
    EXPECT_EQ(hot(fb), 1);

    fb.next->phase.store(CmdPhase::Done); fb.tick();    // done only
    EXPECT_TRUE(fb.done);
    EXPECT_FALSE(fb.busy);
    EXPECT_EQ(hot(fb), 1);
}

// --- 1.2: dropping Execute keeps the block Busy until completion -------------
TEST(CommandFb, StaysBusyAfterExecuteDropped) {
    PokeFb fb;
    fb.next = std::make_shared<CommandStatus>();

    fb.execute = true; fb.tick();
    fb.next->phase.store(CmdPhase::Active); fb.tick();
    ASSERT_TRUE(fb.busy);

    fb.execute = false; fb.tick();        // Execute dropped mid-flight
    EXPECT_TRUE(fb.busy);                 // still busy (command not cancelled)
    EXPECT_FALSE(fb.done);
}

// --- 1.3: terminal visible >= 1 cycle even if Execute already low ------------
TEST(CommandFb, TerminalHeldOneCycleThenResetWhenExecuteLow) {
    PokeFb fb;
    fb.next = std::make_shared<CommandStatus>();

    fb.execute = true; fb.tick();
    fb.execute = false; fb.tick();                      // dropped while running
    fb.next->phase.store(CmdPhase::Done); fb.tick();    // completes with Execute low
    EXPECT_TRUE(fb.done);                               // surfaced for this cycle
    EXPECT_EQ(hot(fb), 1);

    fb.tick();                                          // next cycle, Execute still low
    EXPECT_EQ(hot(fb), 0);                              // now reset
}

// --- 1.3: terminal held as long as Execute stays high ------------------------
TEST(CommandFb, DoneHeldWhileExecuteHigh) {
    PokeFb fb;
    fb.next = std::make_shared<CommandStatus>();

    fb.execute = true; fb.tick();
    fb.next->phase.store(CmdPhase::Done); fb.tick();
    EXPECT_TRUE(fb.done);
    fb.tick(); fb.tick();
    EXPECT_TRUE(fb.done);                               // still held (Execute high)

    fb.execute = false; fb.tick();
    EXPECT_FALSE(fb.done);                              // resets only when Execute low
}

// --- aborted / error surface as the single terminal output -------------------
TEST(CommandFb, AbortSurfaces) {
    PokeFb fb;
    fb.next = std::make_shared<CommandStatus>();
    fb.execute = true; fb.tick();
    fb.next->phase.store(CmdPhase::Aborted); fb.tick();
    EXPECT_TRUE(fb.command_aborted);
    EXPECT_EQ(hot(fb), 1);
}

TEST(CommandFb, SubmitFailureErrors) {
    PokeFb fb;
    fb.next = nullptr;                    // submit returned no status cell
    fb.execute = true; fb.tick();
    EXPECT_TRUE(fb.error);
    EXPECT_EQ(fb.error_id, kErrSubmitFailed);
    EXPECT_EQ(hot(fb), 1);
}

// --- a rising Execute starts a new command, superseding a held result --------
TEST(CommandFb, RisingExecuteSupersedesHeldResult) {
    PokeFb fb;
    fb.next = std::make_shared<CommandStatus>();
    fb.execute = true; fb.tick();
    fb.next->phase.store(CmdPhase::Done); fb.tick();
    ASSERT_TRUE(fb.done);

    fb.execute = false; fb.tick();        // back to idle
    fb.next = std::make_shared<CommandStatus>();
    fb.execute = true; fb.tick();         // new command
    EXPECT_TRUE(fb.busy);
    EXPECT_FALSE(fb.done);
}

// --- write-through round trip through a CommandChannel -----------------------
TEST(CommandRoundTrip, ProviderCompletesThroughChannel) {
    CommandChannel ch;

    struct ChFb : CommandFb {
        CommandChannel* ch = nullptr;
        void tick() {
            const bool r = rising();
            std::shared_ptr<CommandStatus> s;
            if (r) {
                s = std::make_shared<CommandStatus>();
                ch->submit(CommandSubmission{1, 0, "", buffer_mode, s});
            }
            commit(r, s);
        }
        void update() override { tick(); }   // satisfy IFunctionBlock
    };
    ChFb fb; fb.ch = &ch;

    std::vector<CommandSubmission> drained;
    std::weak_ptr<CommandStatus>   active;

    // A minimal provider: drain, mark Active, optionally complete (write-through).
    auto provider = [&](bool complete) {
        ch.drain(drained);
        for (auto& s : drained) {
            if (auto p = s.status.lock()) p->phase.store(CmdPhase::Active);
            active = s.status;
        }
        if (complete) {
            if (auto p = active.lock()) p->phase.store(CmdPhase::Done);
        }
    };

    fb.execute = true; fb.tick();         // FB submits → Busy
    EXPECT_TRUE(fb.busy);

    provider(false);                       // provider receives + marks Active
    fb.tick();
    EXPECT_TRUE(fb.busy);
    EXPECT_TRUE(fb.active);

    provider(true);                        // provider completes
    fb.tick();
    EXPECT_TRUE(fb.done);                   // FB observes completion via write-through
    EXPECT_FALSE(fb.busy);
}

} // namespace
