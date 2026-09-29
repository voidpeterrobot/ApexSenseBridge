#pragma once
#include "apex6/experiment/Session.h"
#include "apex6/experiment/Pulse.h"
#include <istream>
#include <ostream>

namespace asb::apex6::experiment {
inline GripPulseApproval collectPulseConfirmations(std::istream& input,std::ostream& output,const std::string& hash) {
    auto confirm=[&](const char* text){output<<text<<"\nType YES: "<<std::flush;std::string answer;if(!std::getline(input,answer)||answer!="YES")throw ProtocolError("operator did not confirm");};
    GripPulseApproval result;auto& a=result.checkpoints;a.manifestHash=hash;
    confirm("[1/3] READINESS\n"
        "- Silent lifecycle recovery observed; ordinary input and normal vibration work now. Power-cycle recovery checked.\n"
        "- Direct USB, receiver unplugged; official app and known controller writers closed.\n"
        "- Observe throughout; ready to disconnect/power off immediately on unexpected behavior.");
    a.powerCycled=a.directUsb=a.normalVibration=a.observer=a.powerOffReady=a.knownWritersQuiesced=result.silentRecoveryConfirmed=true;
    confirm("[2/3] EXACT PULSE REVIEW AND RISKS\n"
        "- Reviewed two fresh matching baselines and this exact successful offline rehearsal.\n"
        "- ONE 64-ms 125-Hz left-channel burst: gain 12 on original 1/16 base, software peak/limit 0.75, edge peak 0.375.\n"
        "- Three times the previous 4x candidate. Motor force is not calibrated.\n"
        "- 11 waveform packets including neutral lead/tail, four mode writes, 28 queries.\n"
        "- Right grip enabled with neutral data; trigger disabled. Software amplitude is NOT calibrated motor force.\n"
        "- Native host pacing and write deadlines do not guarantee physical stop after host/device failure.\n"
        "- Restore-only captured zero-count/value-1 replies remain UNVERIFIED evidence.\n"
        "- RAM 1/4/5 unacquired; shared access does not prove sole ownership/reply attribution.\n"
        "- Ctrl+C/cancellation or any fault stops ALL traffic, including cleanup; disconnect/power off.\n"
        "- No catch-up, retries, repeats or automatic gain changes. Imperceptible/unclear pulse is inconclusive.\n"
        "- Report pulse location/strength/stop and ordinary input/vibration afterward; abnormal behavior ends testing.");
    a.backgroundRiskAccepted=a.restorationRiskAccepted=a.reducedAuditAccepted=a.failStopAccepted=a.restoreOnlyAccepted=result.pulseAccepted=true;
    output<<"[3/3] Approve only this manifest by typing its full SHA256:\n"<<hash<<"\n> "<<std::flush;
    std::string typed;if(!std::getline(input,typed)||typed!=hash)throw ProtocolError("manifest hash not confirmed");a.approved=true;return result;
}
inline GripLifecycleApproval collectLifecycleConfirmations(std::istream& input,std::ostream& output,const std::string& hash) {
    auto confirm=[&](const char* text){output<<text<<"\nType YES: "<<std::flush;std::string answer;if(!std::getline(input,answer)||answer!="YES")throw ProtocolError("operator did not confirm");};
    GripLifecycleApproval result;auto& a=result.checkpoints;a.manifestHash=hash;
    confirm("[1/3] SETUP AND RECOVERY\n"
        "- Power-cycle recovery after the official capture and ordinary input/vibration checked.\n"
        "- Direct USB connected, receiver unplugged; official software and other known writers closed.\n"
        "- Observe throughout and be ready to disconnect immediately on unexpected movement/behavior.");
    a.powerCycled=a.directUsb=a.normalVibration=a.observer=a.powerOffReady=a.knownWritersQuiesced=true;
    confirm("[2/3] EXACT REVIEW AND RISKS\n"
        "- Reviewed two matching fresh baselines and this exact offline rehearsal. Old approvals are invalid after configuration changes.\n"
        "- Four serial mode writes: grip entry, immediate combined exit, saved left restore, saved right restore.\n"
        "- 28 queries, zero waveform/configuration/trigger writes; no dwell, retries or speculative cleanup.\n"
        "- Only individual restores admit the exact captured zero-count/value-1 envelope as UNVERIFIED evidence.\n"
        "- Unknown, stale, late or missing replies and I/O/trace faults stop all traffic.\n"
        "- Background ownership/reply attribution remain uncertain; RAM 1/4/5 are not acquired.\n"
        "- Matching postflight RAM does not establish recovery. Report ordinary input/vibration and unexpected movement afterward.\n"
        "- Any unexpected behavior ends physical testing; disconnect/power off on failure.");
    a.backgroundRiskAccepted=a.restorationRiskAccepted=a.reducedAuditAccepted=a.failStopAccepted=a.restoreOnlyAccepted=true;
    output<<"[3/3] Approve only this manifest by typing its full SHA256:\n"<<hash<<"\n> "<<std::flush;
    std::string typed;if(!std::getline(input,typed)||typed!=hash)throw ProtocolError("manifest hash not confirmed");a.approved=true;return result;
}
// Prompt collection only: the caller must enforce an interactive console,
// validate the physical review, and timestamp the completed approval.
inline GripRestoreApproval collectRestoreConfirmations(std::istream& input,
    std::ostream& output, const std::string& manifestHash,bool observe=false) {
    auto confirm=[&](const char* text) {
        output<<text<<"\nType YES to confirm ALL items above (anything else cancels): "<<std::flush;
        std::string answer;
        if(!std::getline(input,answer)||answer!="YES")throw ProtocolError("operator did not confirm");
    };
    GripRestoreApproval a;a.manifestHash=manifestHash;
    confirm("\n[1/3] SETUP AND READINESS\n"
        "- Controller power-cycled after the failed neutral test; no streaming since.\n"
        "- Direct USB connected, receiver unplugged; ordinary input and normal vibration checked.\n"
        "- Observe this controller throughout; ready to promptly disconnect/power off.\n"
        "- Known competing controller writers are not intentionally active.");
    a.powerCycled=a.directUsb=a.normalVibration=a.observer=a.powerOffReady=a.knownWritersQuiesced=true;
    if(observe)output<<"\nOBSERVATION POLICY: After one full restore write and its complete first reply,\n"
        "ALL further writes are forbidden. Raw listening may continue for at most\n"
        "500 ms / 32 additional reports, even after an unexpected reply.\n"
        "No postflight or cleanup is sent. A later ACK never proves recovery or resumes writes.\n"
        "Disconnect immediately if behavior is unexpected; otherwise power off after capture.\n";
    confirm(observe?
        "\n[2/3] REVIEW AND RISKS\n"
        "- Accept the bounded receive-only policy above, including listening after an unexpected reply.\n"
        "- Reviewed this exact one-command plan, two matching fresh readbacks and its offline rehearsal.\n"
        "- No streaming entry, waveform, combined exit or right restore.\n"
        "- This command previously returned error 1 both after streaming and after a power cycle.\n"
        "- Motor state may change; shared-access/background ownership and reply attribution remain uncertain.\n"
        "- RAM 1/4/5 are not acquired; full preservation is not established.\n"
        "- No retry or speculative cleanup. Result stays unresolved; power off/disconnect afterward.":
        "\n[2/3] REVIEW AND RISKS\n"
        "- Reviewed this exact plan, two matching fresh readbacks and successful offline rehearsal.\n"
        "- ONE saved left-grip restore only: no streaming entry, waveform, combined exit or right restore.\n"
        "- This same command previously received error 1 and may affect motor state.\n"
        "- Shared-access/background ownership and same-opcode reply attribution remain uncertain.\n"
        "- RAM-6-only audit: RAM 1/4/5 are not acquired; full preservation is not established.\n"
        "- Any unexpected failure stops ALL traffic: no retries or speculative cleanup.\n"
        "  Disconnect/power off on failure or unexpected behavior.");
    a.backgroundRiskAccepted=a.restorationRiskAccepted=a.reducedAuditAccepted=
        a.failStopAccepted=a.restoreOnlyAccepted=true;
    a.observeRepliesAccepted=observe;
    output<<"\n[3/3] Approve only this manifest by typing its full SHA256:\n"<<manifestHash<<"\n> "<<std::flush;
    std::string typed;
    if(!std::getline(input,typed)||typed!=manifestHash)throw ProtocolError("manifest hash not confirmed");
    a.approved=true;
    return a;
}
}
