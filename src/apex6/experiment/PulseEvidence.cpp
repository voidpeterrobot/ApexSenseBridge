#include "apex6/experiment/PulseEvidence.h"
#include "apex6/experiment/Evidence.h"
#include <sstream>
namespace asb::apex6::experiment {
std::string pulseReviewManifest(const GripBaseline& b,const std::string& source,const std::string& executable) {
    const auto active=gripPulsePlan(b);std::ostringstream o;DspMetrics strength;gripPulseFrames(&strength);
    o<<"{\"schema\":\"asb.apex6.grip-pulse-review.v1\",\"scope\":"<<json(gripPulseScope)
     <<",\"source_sha256\":"<<json(source)<<",\"executable_sha256\":"<<json(executable)
     <<",\"fixture_sha256\":"<<json(gripPulseFixtureHash)<<",\"baseline\":"<<json(encodeGripBaseline(b))
     <<",\"signal\":{\"frequency_hz\":125,\"base_peak\":"<<gripPulseBasePeak<<",\"gain\":"<<gripPulseGain<<",\"peak_limit\":"<<gripPulsePeakLimit
     <<",\"strength_policy\":\"fixed_gain_then_symmetric_clip\",\"software_peak\":0.75,\"edge_peak\":0.375,\"tone_ms\":64,\"stream_ms\":88,\"left_byte_tables\":[\"8080808080808080\",\"80a1afa1805e505e\",\"80c3dfc3803c203c\"],\"right_and_trigger_byte\":128,\"enable_byte\":152,\"clipped_samples\":"<<strength.clippedSamples<<",\"overrange_samples\":"<<strength.overrangeSamples<<",\"peak_before_limit\":"<<strength.peakBeforeLimit<<",\"peak_after_limit\":"<<strength.peakAfterLimit<<'}'
     <<",\"limits\":{\"query_writes\":28,\"mode_writes\":4,\"waveform_writes\":11,\"actuator_writes\":15,\"total_writes\":43,\"mode_query_exchange_ms\":600,\"waveform_write_ms\":4,\"packet_spacing_us\":8000,\"max_lateness_us\":2000,\"first_packet_after_ack_ms\":10,\"tail_hold_us\":8000,\"exit_lateness_us\":2000,\"wave_window_ms\":100,\"active_seconds\":5,\"session_seconds\":75,\"preflight_seconds\":30,\"postflight_seconds\":30,\"retries\":0}"
     <<",\"reply_policy\":\"entry/exit normal success ACK only; left/right restore normal ACK or exact captured zero-count/value-1 as unverified evidence; waveform has no ACK; unsolicited waveform-phase input fatal\""
     <<",\"stop_policy\":\"any cancellation, timing, device, trace or I/O fault latches stop with no further traffic or cleanup; no catch-up, dropping, padding, repeats or gain changes\""
     <<",\"wait_policy\":\"prepare before wait; timer wakes 1000 us early; final at-most-1000-us spin checks cancellation and monotonic time; bounded iterations; final device/trace/input/native guards retained\""
     <<",\"uncertainties\":[\"software amplitude is not calibrated motor force\",\"right grip enabled with neutral samples\",\"neutral lead/tail are newly introduced waveform behavior\",\"host deadlines do not bound physical motor duration after device/host failure\",\"restore-envelope firmware meaning unresolved\",\"shared access does not prove sole ownership or reply attribution\",\"RAM 1/4/5 not acquired\",\"matching RAM does not prove recovery\"]"
     <<",\"restoration_verified\":false,\"phases\":{";
    unsigned phase=0;for(const auto& plan:{gripBaselinePlan(),active,gripBaselinePlan()}) {
        if(phase)o<<',';o<<json(phase==0?"preflight":phase==1?"active":"postflight")<<":[";unsigned index=0;
        for(const auto& r:plan) {
            if(index)o<<',';o<<"{\"command\":"<<unsigned(r.command)<<",\"payload_hex\":"<<json(hex(r.payload))<<",\"windows_report_hex\":"<<json(hex(b.binding.layout.wrap(frame(r.command,r.payload))));
            if(phase==1&&r.command==0x57)o<<",\"nominal_due_us\":"<<(index-1)*8000;
            o<<'}';++index;
        }o<<']';++phase;
    }o<<"}}\n";return o.str();
}
std::string pulseResultJson(const GripPulseResult& r) {
    std::ostringstream o;o<<"{\"schema\":\"asb.apex6.grip-pulse.v1\",\"sequence_complete\":"<<(r.sequenceComplete?"true":"false")
     <<",\"waveform_complete\":"<<(r.waveformComplete?"true":"false")<<",\"postflight_matches\":"<<(r.postflightMatches?"true":"false")
     <<",\"query_writes\":"<<r.queryWrites<<",\"mode_writes\":"<<r.modeWrites<<",\"waveform_writes\":"<<r.waveformWrites
     <<",\"restore_replies\":["<<json(modeReplyName(r.restoreReplies[0]))<<','<<json(modeReplyName(r.restoreReplies[1]))<<']'
     <<",\"restoration_verified\":false,\"operator_qualification_pending\":true,\"device_state_uncertain\":"<<(r.deviceStateUncertain?"true":"false")
     <<",\"cancelled\":"<<(r.cancelled?"true":"false")<<",\"failure\":"<<json(r.failure)<<",\"packets\":[";
    bool first=true;for(const auto& p:r.packets){if(!first)o<<',';first=false;o<<"{\"index\":"<<p.index<<",\"native_submit_us\":"<<p.submitted.count()<<",\"native_complete_us\":"<<p.completed.count()<<",\"nominal_us\":"<<p.nominal.count()<<",\"lateness_us\":"<<p.lateness.count()<<",\"spacing_us\":"<<p.spacing.count()<<'}';}
    o<<"],\"dispatches\":[";first=true;
    for(const auto& d:r.dispatches){if(!first)o<<',';first=false;o<<"{\"step\":"<<d.step<<",\"prepared_us\":"<<d.prepared.count()<<",\"due_us\":"<<d.due.count()<<",\"wait_return_us\":"<<d.waitReturned.count()<<",\"checks_complete_us\":"<<d.checksComplete.count()<<'}';}
    o<<"]}\n";return o.str();
}
}
