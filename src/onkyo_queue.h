#pragma once

// The order work is done in, as a thing on its own.
//
// The rules are small and all of them are decisions: a write does not wait
// behind a stale poll, a second volume write replaces the first rather than
// queueing behind it, and a probe goes to the front because somebody is
// looking at a dialog waiting for it. They were spread through five methods of
// a 1750-line class, where the only way to check one was to own a receiver.

#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "phi/adapter/sdk/sidecar.h"

namespace phicore::onkyo::ipc {

struct Operation {
    enum class Kind {
        Poll,
        ChannelInvoke,
        ProbeCurrentInput,
    };

    Kind kind = Kind::Poll;
    std::int64_t enqueuedMs = 0;
    phicore::adapter::sdk::ChannelInvokeRequest channelRequest;
    phicore::adapter::sdk::AdapterActionInvokeRequest actionRequest;
};

class OperationQueue
{
public:
    /**
     * @brief Adds a channel write.
     *
     * @return the command ids of volume writes it replaced. They were never
     *         sent, and the caller owes each of them an answer - phi-core is
     *         waiting on every one.
     */
    std::vector<phicore::adapter::v1::CmdId> pushChannelInvoke(
        const phicore::adapter::sdk::ChannelInvokeRequest &request, std::int64_t nowMs);

    /// To the front: somebody is watching a form field for this.
    void pushProbe(const phicore::adapter::sdk::AdapterActionInvokeRequest &request,
                   std::int64_t nowMs);

    /// At most one poll is ever queued; a newer one replaces the older.
    void pushPoll(std::int64_t nowMs, bool prioritize);

    std::optional<Operation> take();

    /// Empties the queue and hands back what was in it, to be answered.
    std::deque<Operation> drain();

    /// Whether anything ahead of a poll is waiting.
    bool hasPriorityWork() const;

    bool empty() const { return m_queue.empty(); }
    std::size_t size() const { return m_queue.size(); }

private:
    void dropQueuedPolls();

    std::deque<Operation> m_queue;
};

} // namespace phicore::onkyo::ipc
