#include "onkyo_iscp_client.h"

#include <cerrno>
#include <optional>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "phi/runtime/str.h"

namespace phicore::onkyo::ipc {

namespace str = phi::str;

namespace {

/// The wait after the first whole frame, kept only for a command whose answer
/// this adapter cannot recognise. Every command it actually sends is in the
/// table below, so this is the fallback and not the normal path.
constexpr auto kCoalesceWindow = std::chrono::milliseconds(120);

/**
 * @brief What a query asks for, or nothing when it is not a query.
 *
 * A receiver volunteers status nobody asked for, so "a frame arrived" is not
 * "the question was answered". This is how the two are told apart.
 */
std::optional<StatusKind> expectedAnswer(std::string_view command)
{
    if (command == "PWRQSTN")
        return StatusKind::Power;
    if (command == "MVLQSTN")
        return StatusKind::Volume;
    if (command == "AMTQSTN")
        return StatusKind::Mute;
    if (command == "SLIQSTN")
        return StatusKind::Input;
    return std::nullopt;
}

constexpr std::size_t kReadChunk = 4096;

std::string systemError(int code)
{
    return std::string(std::strerror(code));
}

/**
 * @brief One address for `host`, or -1.
 *
 * Numeric hosts resolve without leaving the process. A name does not, and
 * getaddrinfo blocks the loop for the length of the lookup - it is here rather
 * than refused because the alternative, which is what this adapter did before,
 * was to drop every non-numeric host and never connect at all. phi-core
 * resolves an adapter's host in the background and hands the address down, so
 * this path is the window before that lands, not the normal case.
 */
int openConnection(const std::string &host, std::uint16_t port, std::string *error,
                   bool *inProgress)
{
    *inProgress = false;

    ::addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;

    const std::string service = str::number(static_cast<int>(port));
    ::addrinfo *resolved = nullptr;
    int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &resolved);
    if (rc != 0) {
        hints.ai_flags = 0;
        rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &resolved);
    }
    if (rc != 0 || resolved == nullptr) {
        if (error)
            *error = std::string("Cannot resolve host: ") + ::gai_strerror(rc);
        return -1;
    }

    int fd = -1;
    std::string lastError = "No address to connect to";
    for (::addrinfo *entry = resolved; entry != nullptr; entry = entry->ai_next) {
        fd = ::socket(entry->ai_family, entry->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                      entry->ai_protocol);
        if (fd < 0) {
            lastError = systemError(errno);
            continue;
        }
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        if (::connect(fd, entry->ai_addr, entry->ai_addrlen) == 0)
            break;
        if (errno == EINPROGRESS) {
            *inProgress = true;
            break;
        }
        lastError = systemError(errno);
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(resolved);

    if (fd < 0 && error)
        *error = lastError;
    return fd;
}

} // namespace

IscpClient::IscpClient(phi::runtime::Loop &loop)
    : m_loop(loop)
{
}

IscpClient::~IscpClient()
{
    cancel();
}

bool IscpClient::start(IscpRequest request, Done done)
{
    m_loop.assertOnLoop("IscpClient::start");
    if (m_busy)
        return false;
    if (request.hosts.empty() || request.port == 0 || request.commands.empty())
        return false;

    m_request = std::move(request);
    if (m_request.maxAttempts < 1)
        m_request.maxAttempts = 1;
    m_done = std::move(done);
    m_result = IscpResult{};
    m_attempt = 0;
    m_busy = true;
    beginAttempt();
    return true;
}

void IscpClient::cancel()
{
    closeSocket();
    m_done = nullptr;
    m_busy = false;
}

void IscpClient::closeSocket()
{
    m_readWatch.reset();
    m_writeWatch.reset();
    m_deadline.reset();
    m_coalesce.reset();
    if (m_fd >= 0) {
        // Half-close first, and empty what is waiting before letting go.
        //
        // close() on a socket that still has unread data makes the kernel send
        // a reset instead of a finish, and a reset discards whatever of ours
        // has been written but not yet acknowledged. phi-core paid for that
        // once already - a retained sweep reported three topics cleared and
        // left the third one standing - and the command it would cost here is
        // the power write: it is sent with nothing behind it to await, so
        // nothing would notice that it never arrived. A receiver that
        // volunteers status, which they do, is all it takes to fill the
        // receive queue in the meantime.
        //
        // shutdown(SHUT_WR) puts our bytes on the wire with the FIN, so they
        // are gone before any reset can matter. The drain is bounded and
        // non-blocking: it removes the reason for a reset without waiting for
        // a receiver that has decided to keep talking.
        ::shutdown(m_fd, SHUT_WR);
        char discard[2048];
        for (int round = 0; round < 32; ++round) {
            const ssize_t got = ::recv(m_fd, discard, sizeof(discard), MSG_DONTWAIT);
            if (got > 0)
                continue;
            if (got < 0 && errno == EINTR)
                continue;
            break;
        }
        ::close(m_fd);
        m_fd = -1;
    }
    m_inbound.clear();
    m_outbound.clear();
    m_written = 0;
    m_expected.reset();
    m_sawReply = false;
}

void IscpClient::beginAttempt()
{
    ++m_attempt;
    m_candidate = 0;
    m_commandIndex = 0;
    tryNextCandidate();
}

void IscpClient::tryNextCandidate()
{
    closeSocket();

    if (m_candidate >= m_request.hosts.size()) {
        if (m_attempt < m_request.maxAttempts) {
            beginAttempt();
            return;
        }
        if (m_result.error.empty())
            m_result.error = "Receiver unavailable";
        finish();
        return;
    }

    const std::string host = m_request.hosts[m_candidate++];
    std::string error;
    bool inProgress = false;
    const int fd = openConnection(host, m_request.port, &error, &inProgress);
    if (fd < 0) {
        m_result.error = str::arg("%1 (%2:%3)", error, host, m_request.port);
        tryNextCandidate();
        return;
    }

    m_fd = fd;
    m_result.host = host;

    if (!inProgress) {
        onConnectable();
        return;
    }

    m_writeWatch = m_loop.watchFd(m_fd, phi::runtime::Loop::FdEvent::Write,
                                  [this]() { onConnectable(); });
    m_deadline = m_loop.timerAfter(m_request.connectTimeout, [this]() {
        m_result.error = str::arg("Connection timed out (%1:%2)", m_result.host, m_request.port);
        tryNextCandidate();
    });
}

void IscpClient::onConnectable()
{
    int soError = 0;
    ::socklen_t length = sizeof(soError);
    if (::getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &soError, &length) != 0)
        soError = errno;
    if (soError != 0) {
        m_result.error = str::arg("%1 (%2:%3)", systemError(soError), m_result.host,
                                  m_request.port);
        tryNextCandidate();
        return;
    }

    m_writeWatch.reset();
    m_deadline.reset();
    // Reachability is settled here and nowhere else: the exchange that follows
    // can still fail without that saying anything about the receiver being on
    // the network.
    m_result.connected = true;

    m_readWatch = m_loop.watchFd(m_fd, phi::runtime::Loop::FdEvent::Read,
                                 [this]() { onReadable(); });
    sendCurrentCommand();
}

void IscpClient::sendCurrentCommand()
{
    if (m_commandIndex >= m_request.commands.size()) {
        // Complete means every question got its answer. A poll that lost one
        // still carries the other three, and the caller decides what that is
        // worth - `answered` and `unanswered` are there for exactly that.
        m_result.complete = m_result.unanswered == 0;
        finish();
        return;
    }

    const std::string &command = m_request.commands[m_commandIndex];
    m_outbound = kUseEiscp ? encodeEiscpFrame(command) : encodePlainFrame(command);
    m_written = 0;
    m_sawReply = false;
    m_expected = expectedAnswer(command);
    // The buffer is not cleared between commands. Advancing the moment an
    // answer arrives can leave the beginning of the next frame behind, and
    // throwing that away would lose the answer it belongs to.

    while (m_written < m_outbound.size()) {
        const ssize_t sent = ::send(m_fd, m_outbound.data() + m_written,
                                    m_outbound.size() - m_written, MSG_NOSIGNAL);
        if (sent > 0) {
            m_written += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // A status frame is tens of bytes, so this is close to unreachable;
            // it is handled rather than asserted because a socket that says it
            // is full is entitled to be believed.
            m_writeWatch = m_loop.watchFd(m_fd, phi::runtime::Loop::FdEvent::Write,
                                          [this]() {
                                              m_writeWatch.reset();
                                              sendCurrentCommand();
                                          });
            return;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        failAttempt(str::arg("Write failed: %1", systemError(errno)));
        return;
    }

    if (!m_request.awaitReplies) {
        advanceCommand();
        return;
    }

    awaitAnswer();
}

void IscpClient::awaitAnswer()
{
    m_deadline = m_loop.timerAfter(m_request.replyTimeout, [this]() {
        if (m_result.answered == 0) {
            // Nothing has answered anything yet. That is a receiver which is
            // not talking - a Pioneer in standby takes the connection and says
            // nothing - not a query it happens not to know, so there is no
            // point spending a timeout on each of the remaining ones.
            failAttempt(str::arg("No answer to %1", m_request.commands[m_commandIndex]));
            return;
        }
        // It has answered other things, so this is a query this model does not
        // know. The three answers already in hand are worth more than treating
        // the whole poll as failed, which is what a receiver that skips one
        // query used to get.
        ++m_result.unanswered;
        advanceCommand();
    });

    // An answer may already be sitting in the buffer, carried over from the
    // command before.
    consumeInbound();
}

void IscpClient::onReadable()
{
    for (;;) {
        char buffer[kReadChunk];
        const ssize_t got = ::recv(m_fd, buffer, sizeof(buffer), 0);
        if (got > 0) {
            m_inbound.append(buffer, static_cast<std::size_t>(got));
            continue;
        }
        if (got == 0) {
            failAttempt("Receiver closed the connection");
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        failAttempt(str::arg("Read failed: %1", systemError(errno)));
        return;
    }

    consumeInbound();
}

void IscpClient::consumeInbound()
{
    const FrameScan scan = decodeEiscpFrames(m_inbound);
    bool answered = false;
    for (const std::string &payload : scan.payloads) {
        for (Status &status : parseStatuses(payload)) {
            if (m_expected.has_value() && status.kind == *m_expected)
                answered = true;
            m_result.statuses.push_back(std::move(status));
        }
    }
    m_inbound.erase(0, scan.consumed);

    if (scan.corrupt) {
        // Past a length that cannot be believed there is no way to find the
        // next frame, so there is nothing to do but stop reading this stream.
        failAttempt("Malformed frame from the receiver");
        return;
    }
    if (!m_request.awaitReplies)
        return;

    if (m_expected.has_value()) {
        // The question is answered when its own answer arrives, and not before:
        // a volunteered status is not a reply to what was asked.
        if (!answered)
            return;
        ++m_result.answered;
        advanceCommand();
        return;
    }

    // A command whose answer is not in the table. Fall back to what the
    // blocking version did: the first whole frame, then a short wait for the
    // rest of an answer that might have been split.
    if (scan.payloads.empty() || m_sawReply)
        return;
    m_sawReply = true;
    m_deadline.reset();
    m_coalesce = m_loop.timerAfter(kCoalesceWindow, [this]() { advanceCommand(); });
}

void IscpClient::advanceCommand()
{
    m_deadline.reset();
    m_coalesce.reset();
    ++m_commandIndex;
    sendCurrentCommand();
}

void IscpClient::failAttempt(std::string error)
{
    m_result.error = std::move(error);
    // The next candidate, then the next attempt; both run out in
    // tryNextCandidate(), which is also where the request gives up.
    tryNextCandidate();
}

void IscpClient::finish()
{
    closeSocket();
    m_busy = false;
    // Moved out first: the callback is entitled to start the next request from
    // inside itself, and it must find an idle client when it does.
    Done done = std::move(m_done);
    m_done = nullptr;
    IscpResult result = std::move(m_result);
    m_result = IscpResult{};
    if (done)
        done(std::move(result));
}

} // namespace phicore::onkyo::ipc
