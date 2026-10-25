#include "onkyo_queue.h"

namespace phicore::onkyo::ipc {

namespace sdk = phicore::adapter::sdk;
namespace v1 = phicore::adapter::v1;

namespace {

constexpr const char kChannelVolume[] = "volume";

} // namespace

void OperationQueue::dropQueuedPolls()
{
    for (auto it = m_queue.begin(); it != m_queue.end();) {
        if (it->kind == Operation::Kind::Poll)
            it = m_queue.erase(it);
        else
            ++it;
    }
}

std::vector<v1::CmdId> OperationQueue::pushChannelInvoke(
    const sdk::ChannelInvokeRequest &request, std::int64_t nowMs)
{
    // A queued poll is about to be out of date anyway, and a write should not
    // wait for it.
    dropQueuedPolls();

    std::vector<v1::CmdId> superseded;
    if (request.channelExternalId() == kChannelVolume) {
        // A slider produces a stream of these. Only the last one is worth
        // sending; the ones it overtakes are answered by the caller.
        for (auto it = m_queue.begin(); it != m_queue.end();) {
            const bool sameSlider = it->kind == Operation::Kind::ChannelInvoke
                && it->channelRequest.deviceExternalId() == request.deviceExternalId()
                && it->channelRequest.channelExternalId() == kChannelVolume;
            if (!sameSlider) {
                ++it;
                continue;
            }
            superseded.push_back(it->channelRequest.cmdId());
            it = m_queue.erase(it);
        }
    }

    Operation op;
    op.kind = Operation::Kind::ChannelInvoke;
    op.enqueuedMs = nowMs;
    op.channelRequest = request;
    m_queue.push_back(std::move(op));
    return superseded;
}

void OperationQueue::pushMediaPlayerInvoke(const sdk::MediaPlayerInvokeRequest &request,
                                           std::int64_t nowMs)
{
    // Like a channel write, a transport command should not wait for a poll
    // whose result cannot affect it. Unlike a slider, every button press is
    // meaningful and stays in FIFO order.
    dropQueuedPolls();

    Operation op;
    op.kind = Operation::Kind::MediaPlayerInvoke;
    op.enqueuedMs = nowMs;
    op.mediaPlayerRequest = request;
    m_queue.push_back(std::move(op));
}

void OperationQueue::pushProbe(const sdk::AdapterActionInvokeRequest &request, std::int64_t nowMs)
{
    Operation op;
    op.kind = Operation::Kind::ProbeCurrentInput;
    op.enqueuedMs = nowMs;
    op.actionRequest = request;
    m_queue.push_front(std::move(op));
}

void OperationQueue::pushPoll(std::int64_t nowMs, bool prioritize)
{
    dropQueuedPolls();
    Operation op;
    op.kind = Operation::Kind::Poll;
    op.enqueuedMs = nowMs;
    if (prioritize)
        m_queue.push_front(std::move(op));
    else
        m_queue.push_back(std::move(op));
}

std::optional<Operation> OperationQueue::take()
{
    if (m_queue.empty())
        return std::nullopt;
    Operation op = std::move(m_queue.front());
    m_queue.pop_front();
    return op;
}

std::deque<Operation> OperationQueue::drain()
{
    std::deque<Operation> taken;
    taken.swap(m_queue);
    return taken;
}

bool OperationQueue::hasPriorityWork() const
{
    for (const Operation &op : m_queue) {
        if (op.kind != Operation::Kind::Poll)
            return true;
    }
    return false;
}

} // namespace phicore::onkyo::ipc
