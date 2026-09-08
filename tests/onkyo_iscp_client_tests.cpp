// The connection, against a receiver that is not a receiver.
//
// A stub that speaks real eISCP on a loopback port: enough to pin the four
// things the instance depends on and cannot check for itself - that a poll
// batch comes back whole, that a silent receiver costs the reply timeout and
// not the connect timeout, that reachable and answering are separate answers,
// and that a stream this adapter has lost its place in ends rather than hangs.

#include <phi/adapter/testing/check.h>

#include "onkyo_frames.h"
#include "onkyo_iscp_client.h"

#include "phi/runtime/epollloop.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace phicore::onkyo::ipc;
using namespace std::chrono_literals;

namespace {

/// A loopback listener that answers eISCP queries, or does not.
class StubReceiver
{
public:
    enum class Manner {
        Answer,       ///< a proper reply per query
        StaySilent,   ///< accepts the connection and says nothing
        TalkNonsense, ///< a frame whose declared length cannot be believed
        NeverStops    ///< answers, then keeps volunteering status without pause
    };

    explicit StubReceiver(Manner manner) : m_manner(manner)
    {
        m_listen = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        const int one = 1;
        ::setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        m_ok = ::bind(m_listen, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0
            && ::listen(m_listen, 4) == 0;
        socklen_t length = sizeof(addr);
        if (m_ok && ::getsockname(m_listen, reinterpret_cast<sockaddr *>(&addr), &length) == 0)
            m_port = ::ntohs(addr.sin_port);
        m_thread = std::thread([this]() { serve(); });
    }

    ~StubReceiver()
    {
        m_done.store(true);
        ::shutdown(m_listen, SHUT_RDWR);
        ::close(m_listen);
        if (m_thread.joinable())
            m_thread.join();
    }

    bool ok() const { return m_ok; }
    std::uint16_t port() const { return m_port; }
    int queriesSeen() const { return m_queries.load(); }
    /// How the connection ended from this side: EOF is orderly, ECONNRESET is not.
    bool sawOrderlyEnd() const { return m_orderlyEnd.load(); }
    int endErrno() const { return m_endErrno.load(); }

private:
    /// The answer a receiver gives to each of the four queries the poll sends.
    static std::string answerFor(const std::string &command)
    {
        if (command.rfind("PWRQSTN", 0) == 0)
            return "PWR01";
        if (command.rfind("MVLQSTN", 0) == 0)
            return "MVL20";
        if (command.rfind("AMTQSTN", 0) == 0)
            return "AMT00";
        if (command.rfind("SLIQSTN", 0) == 0)
            return "SLI2E";
        return {};
    }

    /// Keeps the client's receive queue occupied for as long as it will take
    /// anything, so there is certainly unread data when it closes.
    void flood(int client)
    {
        std::string burst;
        for (int i = 0; i < 2000; ++i)
            burst += encodeEiscpFrame("PWR01");
        for (int round = 0; round < 200 && !m_done.load(); ++round) {
            const ssize_t sent =
                ::send(client, burst.data(), burst.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            if (sent <= 0)
                break;
        }
    }

    void serve()
    {
        while (!m_done.load()) {
            const int client = ::accept(m_listen, nullptr, nullptr);
            if (client < 0)
                return;
            std::string buffer;
            ssize_t last = 0;
            for (;;) {
                char chunk[512];
                const ssize_t got = ::recv(client, chunk, sizeof(chunk), 0);
                if (got <= 0) {
                    last = got;
                    break;
                }
                buffer.append(chunk, static_cast<std::size_t>(got));

                const FrameScan scan = decodeEiscpFrames(buffer);
                buffer.erase(0, scan.consumed);
                for (const std::string &payload : scan.payloads) {
                    ++m_queries;
                    if (m_manner == Manner::StaySilent)
                        continue;
                    if (m_manner == Manner::TalkNonsense) {
                        // "ISCP", a header size of zero, a payload size of zero:
                        // the frame that used to stop the scanner dead.
                        std::string junk = "ISCP";
                        junk.append(8, '\0');
                        junk.push_back(char(1));
                        junk.append(3, '\0');
                        ::send(client, junk.data(), junk.size(), MSG_NOSIGNAL);
                        continue;
                    }
                    // payload is "!1<command>\r"
                    const std::string command = payload.size() > 2 ? payload.substr(2) : payload;
                    const std::string answer = answerFor(command);
                    if (answer.empty())
                        continue;
                    const std::string frame = encodeEiscpFrame(answer);
                    ::send(client, frame.data(), frame.size(), MSG_NOSIGNAL);
                    if (m_manner == Manner::NeverStops)
                        flood(client);
                }
            }
            if (last == 0)
                m_orderlyEnd.store(true);
            else
                m_endErrno.store(errno);
            ::close(client);
        }
    }

    Manner m_manner;
    std::atomic_bool m_orderlyEnd{false};
    std::atomic_int m_endErrno{0};
    int m_listen = -1;
    bool m_ok = false;
    std::uint16_t m_port = 0;
    std::atomic_bool m_done{false};
    std::atomic_int m_queries{0};
    std::thread m_thread;
};

/// Runs one request to completion on a real loop and answers with its result.
std::optional<IscpResult> run(IscpRequest request, std::chrono::milliseconds budget = 8000ms)
{
    phi::runtime::EpollLoop loop;
    IscpClient client(loop);
    std::optional<IscpResult> result;

    const bool started = client.start(std::move(request), [&](IscpResult done) {
        result = std::move(done);
        loop.stop();
    });
    if (!started)
        return std::nullopt;

    phi::runtime::Timer watchdog = loop.timerAfter(budget, [&loop]() { loop.stop(); });
    loop.run();
    return result;
}

IscpRequest pollRequest(std::uint16_t port)
{
    IscpRequest request;
    request.hosts = {"127.0.0.1"};
    request.port = port;
    request.commands = {"PWRQSTN", "MVLQSTN", "AMTQSTN", "SLIQSTN"};
    request.replyTimeout = 500ms;
    request.connectTimeout = 1500ms;
    return request;
}

void testAPollBatchComesBackWhole()
{
    StubReceiver receiver(StubReceiver::Manner::Answer);
    PHI_CHECK(receiver.ok());

    const auto result = run(pollRequest(receiver.port()));
    PHI_CHECK_MSG(result.has_value(), "the request never finished");
    if (!result)
        return;
    PHI_CHECK(result->connected);
    PHI_CHECK_MSG(result->complete, "incomplete: %s", result->error.c_str());
    PHI_CHECK(result->host == "127.0.0.1");
    PHI_CHECK_MSG(receiver.queriesSeen() == 4, "receiver saw %d queries", receiver.queriesSeen());

    PHI_CHECK_MSG(result->statuses.size() == 4, "got %d statuses", int(result->statuses.size()));
    if (result->statuses.size() == 4) {
        PHI_CHECK(result->statuses[0].kind == StatusKind::Power && result->statuses[0].flag);
        PHI_CHECK(result->statuses[1].kind == StatusKind::Volume
                  && result->statuses[1].raw == 0x20);
        PHI_CHECK(result->statuses[2].kind == StatusKind::Mute && !result->statuses[2].flag);
        PHI_CHECK(result->statuses[3].kind == StatusKind::Input
                  && result->statuses[3].code == "2E");
    }
}

void testReachableAndAnsweringAreDifferentAnswers()
{
    // The receiver takes the connection and says nothing - a Pioneer in standby
    // does exactly this, accepting the socket and hanging up without answering.
    //
    // It still counts as connected, and that is the decision rather than an
    // accident: a receiver you can reach is a receiver you can switch on, so
    // the UI must offer it. Only a connection that cannot be made is
    // disconnected. Whether the exchange then worked is a separate fact, and
    // the instance reports it on separate channels - power stays Unknown.
    StubReceiver receiver(StubReceiver::Manner::StaySilent);
    PHI_CHECK(receiver.ok());

    IscpRequest request = pollRequest(receiver.port());
    request.replyTimeout = 200ms;
    request.connectTimeout = 3000ms;

    const auto before = std::chrono::steady_clock::now();
    const auto result = run(request);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - before);

    PHI_CHECK(result.has_value());
    if (!result)
        return;
    PHI_CHECK_MSG(result->connected, "a completed TCP connect was not reported");
    PHI_CHECK(!result->complete);
    PHI_CHECK(!result->error.empty());
    // The silence costs one reply timeout, not the connect budget.
    PHI_CHECK_MSG(elapsed < 1500ms, "a silent receiver cost %lldms",
                  static_cast<long long>(elapsed.count()));
}

void testNothingListeningIsNotReachable()
{
    // The other half of the rule: no connection, no connectivity. There is
    // nothing to switch on at a port nothing is listening to.
    //
    // Bind and close, so the port is one nothing answers on.
    std::uint16_t deadPort = 0;
    {
        StubReceiver probe(StubReceiver::Manner::Answer);
        PHI_CHECK(probe.ok());
        deadPort = probe.port();
    }

    const auto result = run(pollRequest(deadPort));
    PHI_CHECK(result.has_value());
    if (!result)
        return;
    PHI_CHECK_MSG(!result->connected, "an unreachable port reported a connection");
    PHI_CHECK(!result->complete);
    PHI_CHECK_MSG(result->error.find(std::to_string(deadPort)) != std::string::npos,
                  "the error does not name the endpoint: %s", result->error.c_str());
}

void testTheFirstCandidateThatAnswersIsUsed()
{
    std::uint16_t deadPort = 0;
    {
        StubReceiver probe(StubReceiver::Manner::Answer);
        deadPort = probe.port();
    }
    StubReceiver live(StubReceiver::Manner::Answer);
    PHI_CHECK(live.ok());

    IscpRequest request = pollRequest(live.port());
    // Same port for both entries would prove nothing; the dead one is reached
    // through a host that resolves and refuses.
    request.hosts = {"127.0.0.2", "127.0.0.1"};
    request.port = live.port();
    (void)deadPort;

    const auto result = run(request);
    PHI_CHECK(result.has_value());
    if (!result)
        return;
    PHI_CHECK_MSG(result->complete, "fell through both candidates: %s", result->error.c_str());
    PHI_CHECK(result->host == "127.0.0.1");
}

void testAStreamWeHaveLostOurPlaceInEndsRatherThanHangs()
{
    // The frame that used to put the instance thread into a loop it never left.
    // Here it has to come back, and say why.
    StubReceiver receiver(StubReceiver::Manner::TalkNonsense);
    PHI_CHECK(receiver.ok());

    IscpRequest request = pollRequest(receiver.port());
    request.replyTimeout = 300ms;

    const auto before = std::chrono::steady_clock::now();
    const auto result = run(request, 5000ms);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - before);

    PHI_CHECK_MSG(result.has_value(), "the request never finished - the scan stood still");
    if (!result)
        return;
    PHI_CHECK(result->connected);
    PHI_CHECK(!result->complete);
    PHI_CHECK_MSG(elapsed < 3000ms, "took %lldms to give up",
                  static_cast<long long>(elapsed.count()));
}

void testTheConnectionIsLetGoOfRatherThanTornDown()
{
    // Closing a socket that still has unread data on it makes the kernel send a
    // reset instead of a finish, and a reset discards whatever of ours has not
    // been acknowledged yet. phi-core paid for this once already: a retained
    // sweep reported three topics cleared and left the third one standing.
    //
    // Here it would be the power command. A receiver that volunteers status -
    // which they do - can have filled our receive queue by the time the write
    // is done, and "switch the receiver on" is the one command with nothing
    // behind it to notice that it never arrived.
    StubReceiver receiver(StubReceiver::Manner::NeverStops);
    PHI_CHECK(receiver.ok());

    IscpRequest request = pollRequest(receiver.port());
    request.commands = {"PWRQSTN"};

    const auto result = run(request);
    PHI_CHECK(result.has_value());
    if (!result)
        return;
    PHI_CHECK(result->connected);

    // This passes with a bare close() as well: on loopback the client drains
    // whatever it reads, so the two conditions a reset needs - our bytes not
    // yet delivered, and their bytes not yet read - do not coincide here. What
    // it pins is the invariant, so a later change that stops draining, or that
    // closes without the half-close, is caught. The evidence that the kernel
    // does send a reset in that situation is a four-line C program, not this.
    std::this_thread::sleep_for(200ms);
    PHI_CHECK_MSG(receiver.sawOrderlyEnd(),
                  "the receiver saw errno %d instead of an orderly end", receiver.endErrno());
}

} // namespace

int main()
{
    testAPollBatchComesBackWhole();
    testReachableAndAnsweringAreDifferentAnswers();
    testNothingListeningIsNotReachable();
    testTheFirstCandidateThatAnswersIsUsed();
    testAStreamWeHaveLostOurPlaceInEndsRatherThanHangs();
    testTheConnectionIsLetGoOfRatherThanTornDown();
    return phi::testing::report("onkyo_iscp_client_tests");
}
