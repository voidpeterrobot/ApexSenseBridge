// SPDX-FileCopyrightText: 2026 Mikalai Kaliaha
// SPDX-License-Identifier: MIT
#pragma once
#include "apex6/Gpa6.h"
#include <chrono>
#include <functional>
#include <string>
#include <atomic>
#include <memory>

namespace asb::apex6::experiment {
enum class AccessMode { Unknown, Exclusive, Shared, Synthetic };
std::string accessName(AccessMode);
AccessMode parseAccess(const std::string&);
using Time = std::chrono::microseconds;
struct Layout {
    std::uint8_t inputId = 0, outputId = 0;
    std::uint16_t inputLength = 33, outputLength = 33; // includes Windows ID byte
    void validate() const;
    Bytes wrap(const Frame&) const;
    Bytes unwrap(std::span<const std::uint8_t>) const;
    bool operator==(const Layout&) const = default;
};
struct Binding {
    std::string path, instance, container, descriptorSignature;
    Layout layout;
    AccessMode access = AccessMode::Unknown;
    bool operator==(const Binding&) const = default;
};
struct Identity {
    std::uint8_t deviceType{}, connection{}, features{};
    std::array<std::uint16_t, 7> firmware{};
    bool operator==(const Identity&) const = default;
};
Identity identity(std::span<const std::uint8_t>);
using Uid = std::array<std::uint8_t, 16>;
Uid uid(std::span<const std::uint8_t>);
std::array<std::uint16_t, 3> versions(std::span<const std::uint8_t>);
struct Snapshot {
    unsigned schema = 2;
    Binding binding;
    Identity info;
    Uid unit{};
    std::array<std::uint16_t, 3> formats{};
    ConfigState config{};
    std::array<RamFingerprint, 4> fingerprints{};
    std::array<Bytes, 4> blocks;
    bool operator==(const Snapshot&) const = default;
};
std::string hex(std::span<const std::uint8_t>);
Bytes unhex(const std::string&);
std::string encodeSnapshot(const Snapshot&);
Snapshot decodeSnapshot(const std::string&); // canonical bounded format; no permissive JSON parsing
void validateSnapshot(const Snapshot&);

// Deliberately separate from the full RAM 1/4/5/6 audit. Never encode this as
// a Snapshot or accept it through the existing physical neutral authorization.
struct GripBaseline {
    unsigned schema = 1;
    bool physicalOrigin = false;
    Binding binding;
    Identity info;
    Uid unit{};
    std::array<std::uint16_t, 3> formats{};
    ConfigState config{};
    RamFingerprint motor{};
    Bytes mapping;
    bool operator==(const GripBaseline&) const = default;
};
void validateGripBaseline(const GripBaseline&);
std::string encodeGripBaseline(const GripBaseline&);
GripBaseline decodeGripBaseline(const std::string&);

enum class Completion { Complete, Idle, Timeout, Failed, Unresolved };
struct IoResult {
    Completion status; Bytes bytes; std::size_t transferred = 0; std::uint32_t error = 0;
    std::optional<Time> submittedAt,completedAt; // native write timestamps, required by the pulse runner
};
class Io {
public:
    virtual ~Io() = default;
    virtual Time now() const = 0;
    virtual bool physical() const = 0;
    virtual const Binding& binding() const = 0;
    virtual bool stillSameDevice() = 0;
    virtual IoResult read(Time absoluteDeadline, bool poll) = 0;
    virtual IoResult write(std::span<const std::uint8_t>, Time absoluteDeadline) = 0;
};
class Trace {
public:
    virtual ~Trace() = default;
    virtual void record(Time, const std::string& event, std::span<const std::uint8_t> raw = {},
                        std::uint64_t operation = 0, std::uint64_t detail = 0) = 0;
    virtual bool healthy() const = 0;
};
struct Request {
    std::uint8_t command;
    Bytes payload;
    std::size_t replySize;
    bool v21 = false, variableChunk = false;
    bool operator==(const Request&) const = default;
};
std::vector<Request> snapshotPlan(const Snapshot&);
std::vector<Request> neutralPlan(const Snapshot&);
std::vector<Request> gripBaselinePlan(); // fixed 14 queries; RAM 6 only
std::vector<Request> neutralPlan(const GripBaseline&);
std::vector<Request> gripRestoreLeftPlan(const GripBaseline&); // ONE saved-left mode command
std::vector<Request> gripLifecyclePlan(const GripBaseline&);
struct GripLifecycleResult {
    bool sequenceComplete=false,postflightMatches=false,restorationVerified=false,deviceStateUncertain=false;
    std::array<ModeReply,2> restoreReplies{ModeReply::NotObserved,ModeReply::NotObserved};
    std::string failure;
};

struct RunResult;
struct RestoreObservation {
    bool started=false,complete=false;
    unsigned reports=0;
    Time elapsed{};
    std::string firstReply,stop="not_started",error;
};
class Session {
public:
    Session(Io&, Trace&);
    void begin(const std::string& phase, Time duration, unsigned attempts,
               std::vector<Request> exactPlan = {});
    Bytes exchange(const Request&);
    void end();
    [[noreturn]] void stop(const std::string&);
    void guard();
    Time now() const { return io_.now(); }
    const Binding& binding() const { return binding_; }
    bool physical() const { return io_.physical(); }
    bool failed() const { return !failure_.empty(); }
    const std::string& failure() const { return failure_; }
    unsigned attempts() const { return attempts_; }
    bool uncertain() const { return uncertain_; }
    unsigned queryAttempts() const { return queries_; }
    unsigned actuatorAttempts() const { return actuators_; }
    const RestoreObservation& observation() const { return observation_; }
private:
    friend class GripPulseRunner;
    friend GripLifecycleResult runGripLifecycle(Session&,const GripBaseline&);
    friend GripLifecycleResult runAuthorizedGripLifecycle(Session&,const class GripLifecycleAuthorization&,const std::function<std::int64_t()>&);
    friend RunResult runAuthorizedNeutral(Session&, const class NeutralAuthorization&, const std::function<std::int64_t()>&);
    friend RunResult runAuthorizedGripNeutral(Session&, const class GripNeutralAuthorization&, const std::function<std::int64_t()>&);
    friend RunResult runAuthorizedGripRestore(Session&, const class GripRestoreAuthorization&, const std::function<std::int64_t()>&);
    friend RunResult runNeutralLifecycle(Session&, const Snapshot&);
    friend RunResult runGripRestoreLifecycle(Session&,const GripBaseline&,bool);
    void observeRestoreTail();
    Io& io_; Trace& trace_; Binding binding_;
    Time last_{}, deadline_{}, totalDeadline_{};
    std::vector<Request> plan_;
    std::size_t index_ = 0;
    unsigned attempts_ = 0, budget_ = 0, modes_ = 0, waves_ = 0;
    std::uint64_t operation_ = 0;
    bool begun_ = false, ended_ = false, uncertain_ = false;
    std::string failure_;
    unsigned queries_=0,actuators_=0;
    std::vector<Request> authorizedActive_;
    std::function<void()> beforeEntry_;
    bool observationEligible_=false;
    RestoreObservation observation_;
    bool lifecyclePolicy_=false;
    ModeReply lastModeReply_=ModeReply::NotObserved;
    std::function<void()> scopedGuard_; // installed only for the separately authorized pulse
};
Snapshot acquireSnapshot(Session&);
// Owns its exact-plan phase; query-only, even for physical Io.
GripBaseline acquireGripBaseline(Session&);
struct RunResult { bool complete = false, deviceStateUncertain = false; std::string failure; };
// Deliberately refuses physical Io before the first request. Hardware-neutral
// execution remains locked pending fresh baseline and separate review.
RunResult rehearseNeutral(Session&, const Snapshot&);
// Rehearsal never accepts physical Io; scoped execution requires its own authority.
RunResult rehearseNeutral(Session&, const GripBaseline&);
struct Approval {
    std::string manifestHash;
    std::int64_t confirmedUnixSeconds = 0;
    bool approved = false, observer = false, powerOffReady = false, normalVibration = false,
        knownWritersQuiesced = false, directUsb = false, allDisabledAccepted = false, failStopAccepted = false,
        backgroundRiskAccepted = false, restorationRiskAccepted = false;
};
bool validApproval(const Approval&, const std::string& hash, std::int64_t nowUnixSeconds);
std::string encodeApproval(const Approval&);
Approval decodeApproval(const std::string&);
class NeutralAuthorization {
public:
    static NeutralAuthorization approve(const Snapshot&,const Approval&,const std::string& manifestHash,std::int64_t now);
    const Snapshot& baseline() const { return baseline_; }
    void check(std::int64_t now) const;
private:
    Snapshot baseline_; Approval approval_; std::string hash_;
};
RunResult runAuthorizedNeutral(Session&,const NeutralAuthorization&,const std::function<std::int64_t()>& clock);
struct GripApproval {
    Approval checkpoints;
    bool reducedAuditAccepted = false;
};
std::string encodeGripApproval(const GripApproval&);
GripApproval decodeGripApproval(const std::string&);
class GripNeutralAuthorization {
public:
    static GripNeutralAuthorization approve(const GripBaseline&,const GripApproval&,const std::string& manifestHash,std::int64_t now);
    const GripBaseline& baseline() const { return baseline_; }
    void check(std::int64_t now) const;
private:
    GripBaseline baseline_;GripApproval approval_;std::string hash_;
};
RunResult runAuthorizedGripNeutral(Session&,const GripNeutralAuthorization&,const std::function<std::int64_t()>& clock);
// Separate contract: no streaming entry/exit, waveform, right restoration or retries.
struct GripRestoreApproval {
    std::string manifestHash;
    std::int64_t confirmedUnixSeconds = 0;
    bool approved=false,observer=false,powerOffReady=false,normalVibration=false,
        knownWritersQuiesced=false,directUsb=false,failStopAccepted=false,
        backgroundRiskAccepted=false,restorationRiskAccepted=false,reducedAuditAccepted=false,
        powerCycled=false,restoreOnlyAccepted=false;
    bool observeRepliesAccepted=false; // distinct token and manifest; never implicit
};
std::string encodeGripRestoreApproval(const GripRestoreApproval&);
GripRestoreApproval decodeGripRestoreApproval(const std::string&);
class GripRestoreAuthorization {
public:
    static GripRestoreAuthorization approve(const GripBaseline&,const GripRestoreApproval&,const std::string& manifestHash,std::int64_t now,bool observe=false);
    const GripBaseline& baseline() const { return baseline_; }
    void check(std::int64_t now) const;
    bool observeReplies() const { return approval_.observeRepliesAccepted; }
private:
    GripBaseline baseline_;GripRestoreApproval approval_;std::string hash_;
};
RunResult rehearseGripRestore(Session&,const GripBaseline&,bool observe=false);
RunResult runAuthorizedGripRestore(Session&,const GripRestoreAuthorization&,const std::function<std::int64_t()>& clock);
inline constexpr char gripLifecycleScope[]="grip-mode-only-lifecycle-v1";
struct GripLifecycleApproval { GripRestoreApproval checkpoints; };
std::string encodeGripLifecycleApproval(const GripLifecycleApproval&);
GripLifecycleApproval decodeGripLifecycleApproval(const std::string&);
class GripLifecycleAuthorization {
public:
    static GripLifecycleAuthorization approve(const GripBaseline&,const GripLifecycleApproval&,const std::string&,std::int64_t);
    const GripBaseline& baseline()const{return baseline_;}
    void check(std::int64_t)const;
    void consume()const; // shared across copies; one native transport only
private:
    GripBaseline baseline_;GripLifecycleApproval approval_;std::string hash_;
    std::shared_ptr<std::atomic_bool> consumed_=std::make_shared<std::atomic_bool>(false);
};
GripLifecycleResult rehearseGripLifecycle(Session&,const GripBaseline&);
GripLifecycleResult runAuthorizedGripLifecycle(Session&,const GripLifecycleAuthorization&,const std::function<std::int64_t()>&);
}
