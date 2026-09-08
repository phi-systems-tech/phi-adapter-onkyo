#pragma once

// The connection to a receiver: connect, send, read, in one place.
//
// It used to be two nearly identical five-hundred-line lambdas inside the
// instance, both built out of QTcpSocket::waitFor... - and that is what made
// the documented poll preemption fiction. Instance callbacks arrive as posted
// work on the same thread, and a socket wait does not run posted work, so the
// only thing that could ever have interrupted a poll was starved by the poll
// it was meant to interrupt. Nothing here waits: the loop delivers readability
// and everything else keeps running between the parts.
//
// What goes on the wire is deliberately unchanged - one command, its answer,
// then the next - so that leaving Qt cannot be what breaks a receiver. What
// changed is when a command counts as answered: the answer to it, rather than
// any frame at all followed by a fixed wait in case more was coming. The
// blocking version could not tell a whole frame from half of one, so the wait
// was the only way to be sure; the scanner can, so it is not needed. On the
// measured receiver that wait was 480 of the poll's 1048 milliseconds.

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "phi/runtime/loop.h"

#include "onkyo_frames.h"

namespace phicore::onkyo::ipc {

struct IscpRequest {
    /// Tried in order; the first that accepts a connection is used.
    std::vector<std::string> hosts;
    std::uint16_t port = 0;
    /// Sent one at a time, each awaited before the next goes out.
    std::vector<std::string> commands;
    /// False for a write with nothing to read back (a power command).
    bool awaitReplies = true;
    std::chrono::milliseconds connectTimeout{1500};
    std::chrono::milliseconds replyTimeout{500};
    /// A whole-request retry, from the first candidate again.
    int maxAttempts = 1;
};

struct IscpResult {
    /// TCP came up on one of the candidates. Separate from `complete` because
    /// reachability and a working exchange are different facts, and the
    /// connectivity channel reports the first one.
    bool connected = false;
    /// Every command went out and, where awaited, was answered.
    bool complete = false;
    /// Awaited commands that got the answer they asked for.
    int answered = 0;
    /**
     * @brief Awaited commands the receiver did not answer.
     *
     * Not the same as a failure. Model lines differ in which queries they know,
     * and one query going unanswered is no reason to throw away the three that
     * did answer - so the exchange carries on and the caller decides. It only
     * gives up when *nothing* has answered, because that is a receiver that is
     * not talking rather than a query it does not recognise.
     */
    int unanswered = 0;
    std::string host;
    std::string error;
    std::vector<Status> statuses;
};

/**
 * @brief One exchange at a time, driven by the loop it was built on.
 *
 * Owned by, and used from, the thread whose loop it holds. The completion
 * callback runs on that thread and may start the next request from inside
 * itself.
 */
class IscpClient
{
public:
    using Done = std::function<void(IscpResult)>;

    explicit IscpClient(phi::runtime::Loop &loop);
    ~IscpClient();

    IscpClient(const IscpClient &) = delete;
    IscpClient &operator=(const IscpClient &) = delete;

    bool busy() const { return m_busy; }

    /// False when one is already in flight; `done` is then never called.
    bool start(IscpRequest request, Done done);

    /// Drops whatever is in flight without calling `done`.
    void cancel();

private:
    void beginAttempt();
    void tryNextCandidate();
    void onConnectable();
    void onReadable();
    void sendCurrentCommand();
    void consumeInbound();
    void awaitAnswer();
    void advanceCommand();
    void failAttempt(std::string error);
    void finish();
    void closeSocket();

    phi::runtime::Loop &m_loop;
    Done m_done;
    IscpRequest m_request;
    IscpResult m_result;

    int m_fd = -1;
    phi::runtime::FdWatch m_readWatch;
    phi::runtime::FdWatch m_writeWatch;
    phi::runtime::Timer m_deadline;
    phi::runtime::Timer m_coalesce;

    std::size_t m_candidate = 0;
    std::size_t m_commandIndex = 0;
    int m_attempt = 0;
    std::string m_inbound;
    std::string m_outbound;
    std::size_t m_written = 0;
    /// The status the command in flight asked for; empty for a command whose
    /// answer this adapter does not know how to recognise, which falls back to
    /// "the first whole frame, then a short wait".
    std::optional<StatusKind> m_expected;
    bool m_sawReply = false;
    bool m_busy = false;
};

} // namespace phicore::onkyo::ipc
