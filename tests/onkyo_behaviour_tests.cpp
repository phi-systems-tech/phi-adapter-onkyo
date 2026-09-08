// The three decisions that used to be spread through the instance: what is
// worth reporting, what an input is called, and what order work happens in.
// None of them need a receiver, and none of them could be checked without one
// while they lived inside a 1750-line class.

#include <phi/adapter/testing/check.h>

#include "onkyo_inputs.h"
#include "onkyo_json.h"
#include "onkyo_queue.h"
#include "onkyo_state.h"

#include <string>

using namespace phicore::onkyo::ipc;
namespace sdk = phicore::adapter::sdk;

namespace {

Status power(bool on) { return Status{StatusKind::Power, on, 0, {}}; }
Status volumeRaw(int raw) { return Status{StatusKind::Volume, false, raw, {}}; }
Status input(const std::string &code) { return Status{StatusKind::Input, false, 0, code}; }

sdk::ChannelInvokeRequest invoke(std::uint64_t cmdId, const char *channel)
{
    sdk::ChannelInvokeRequest request;
    request.cmdId = cmdId;
    request.deviceExternalId = "dev";
    request.channelExternalId = channel;
    return request;
}

void testOnlyNewsIsReported()
{
    ReceiverState state;
    state.setVolumeMaxRaw(160);

    PHI_CHECK(state.power() == PowerState::Unknown);
    PHI_CHECK(state.apply({power(true), volumeRaw(0x20)}).size() == 2);
    PHI_CHECK(state.power() == PowerState::On);

    // The same poll again is not an event. Four channels reported five times a
    // minute, forever, is what deduplication is for.
    PHI_CHECK_MSG(state.apply({power(true), volumeRaw(0x20)}).empty(),
                  "an unchanged poll was reported as a change");

    const auto changed = state.apply({power(false)});
    PHI_CHECK(changed.size() == 1 && changed.front().kind == StatusKind::Power
              && !changed.front().flag);
    PHI_CHECK(state.power() == PowerState::Off);
}

void testVolumeScalesBothWays()
{
    ReceiverState state;
    state.setVolumeMaxRaw(160);
    PHI_CHECK(state.percentForRaw(0) == 0);
    PHI_CHECK(state.percentForRaw(160) == 100);
    PHI_CHECK(state.percentForRaw(80) == 50);
    PHI_CHECK(state.rawForPercent(50.0) == 80);
    PHI_CHECK(state.rawForPercent(100.0) == 160);
    // Out of range on either side is the nearest end, not a wrap.
    PHI_CHECK(state.rawForPercent(-20.0) == 0);
    PHI_CHECK(state.rawForPercent(400.0) == 160);
    PHI_CHECK(state.percentForRaw(999) == 100);

    // A receiver on a different scale.
    state.setVolumeMaxRaw(100);
    PHI_CHECK(state.percentForRaw(50) == 50);
    // And one configured beyond what the field allows.
    state.setVolumeMaxRaw(100000);
    PHI_CHECK(state.volumeMaxRaw() == 500);
}

void testForgettingLetsTheSameValueBeReportedAgain()
{
    ReceiverState state;
    PHI_CHECK(state.apply({power(true), input("2E")}).size() == 2);
    PHI_CHECK(state.apply({power(true)}).empty());

    // Connectivity was lost. What was true of the old connection says nothing
    // about the new one, so the next poll has to be allowed to say it again.
    state.forget();
    PHI_CHECK(state.power() == PowerState::Unknown);
    PHI_CHECK(state.apply({power(true)}).size() == 1);
    // Except the last input, which is the answer "probe current" gives when the
    // receiver has gone quiet.
    PHI_CHECK(state.lastInputCode() == "2E");
}

void testAnInputIsCalledWhateverIsMostSpecific()
{
    InputCatalog catalog;
    catalog.setDefaults({{"05", "Blu-ray"}, {"2E", "BT Audio"}, {"10", "DVD"}});
    catalog.applyMeta(parseObject(R"({
        "activeSliCodes": ["2E", "05", "07"],
        "inputLabel_05": "Beamer"
    })"));

    PHI_CHECK(catalog.label("05") == "Beamer");    // what the user typed
    PHI_CHECK(catalog.label("2E") == "BT Audio");  // what the factory knows
    PHI_CHECK(catalog.label("07") == "SLI 07");    // neither

    // Configured order, not sorted and not hashed: the same configuration must
    // produce the same list twice, which a QHash's key order did not.
    const auto choices = catalog.choices();
    PHI_CHECK_MSG(choices.size() == 3, "%d choices", int(choices.size()));
    if (choices.size() == 3) {
        PHI_CHECK(choices[0].value == "2E");
        PHI_CHECK(choices[1].value == "05");
        PHI_CHECK(choices[2].value == "07");
        PHI_CHECK(choices[1].label == "Beamer");
    }

    // Somebody typing a name instead of a number.
    PHI_CHECK(catalog.codeForLabel("beamer") == "05");
    PHI_CHECK(catalog.codeForLabel("  BT Audio ") == "2E");
    PHI_CHECK(catalog.codeForLabel("nothing here").empty());

    // Nothing configured yet: everything the factory has a name for beats an
    // empty selector.
    InputCatalog fresh;
    fresh.setDefaults({{"05", "Blu-ray"}, {"10", "DVD"}});
    fresh.applyMeta(parseObject("{}"));
    PHI_CHECK(fresh.choices().size() == 2);
}

void testAWriteDoesNotWaitBehindAStalePoll()
{
    OperationQueue queue;
    queue.pushPoll(1, false);
    PHI_CHECK(queue.size() == 1);

    PHI_CHECK(queue.pushChannelInvoke(invoke(10, "mute"), 2).empty());
    PHI_CHECK_MSG(queue.size() == 1, "the poll survived a write");
    PHI_CHECK(queue.hasPriorityWork());

    const auto taken = queue.take();
    PHI_CHECK(taken.has_value() && taken->kind == Operation::Kind::ChannelInvoke);
    PHI_CHECK(!queue.take().has_value());
}

void testASliderSendsItsLastPositionAndAnswersTheRest()
{
    OperationQueue queue;
    PHI_CHECK(queue.pushChannelInvoke(invoke(1, "volume"), 10).empty());
    PHI_CHECK(queue.pushChannelInvoke(invoke(2, "mute"), 11).empty());

    // The third volume write overtakes the first. phi-core is waiting on cmdId
    // 1, so the caller is handed it back to answer - dropping it silently is a
    // command that never returns.
    const auto superseded = queue.pushChannelInvoke(invoke(3, "volume"), 12);
    PHI_CHECK_MSG(superseded.size() == 1, "%d superseded", int(superseded.size()));
    if (superseded.size() == 1)
        PHI_CHECK(superseded.front() == 1);
    PHI_CHECK(queue.size() == 2);

    // The mute in between is untouched, and still ahead of the volume.
    const auto first = queue.take();
    PHI_CHECK(first.has_value() && first->channelRequest.cmdId == 2);
    const auto second = queue.take();
    PHI_CHECK(second.has_value() && second->channelRequest.cmdId == 3);
}

void testAProbeGoesToTheFrontAndNothingIsLostOnADrain()
{
    OperationQueue queue;
    queue.pushChannelInvoke(invoke(1, "mute"), 10);
    queue.pushPoll(11, false);

    sdk::AdapterActionInvokeRequest probe;
    probe.cmdId = 99;
    probe.actionId = "probeCurrentInput";
    // Somebody is watching a form field for this one.
    queue.pushProbe(probe, 12);

    const auto first = queue.take();
    PHI_CHECK(first.has_value() && first->kind == Operation::Kind::ProbeCurrentInput);

    const auto rest = queue.drain();
    PHI_CHECK_MSG(rest.size() == 2, "%d operations dropped on the floor", int(rest.size()));
    PHI_CHECK(queue.empty());
    PHI_CHECK(!queue.hasPriorityWork());
}

} // namespace

int main()
{
    testOnlyNewsIsReported();
    testVolumeScalesBothWays();
    testForgettingLetsTheSameValueBeReportedAgain();
    testAnInputIsCalledWhateverIsMostSpecific();
    testAWriteDoesNotWaitBehindAStalePoll();
    testASliderSendsItsLastPositionAndAnswersTheRest();
    testAProbeGoesToTheFrontAndNothingIsLostOnADrain();
    return phi::testing::report("onkyo_behaviour_tests");
}
