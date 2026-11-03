// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "station_intent.hpp"

namespace prd::network::station {
namespace {

using app_model::NetworkPhase;

// Unsigned throughout, so a monotonic clock that wraps produces a small elapsed time rather than
// an enormous one. The same arithmetic provisioning_flow uses for its dwell and timeout.
[[nodiscard]] std::uint32_t elapsed_since(const std::uint32_t start, const std::uint32_t now) {
    return now - start;
}

[[nodiscard]] Fault fault_for(const DisconnectCause cause) {
    return cause == DisconnectCause::CredentialsRejected ? Fault::CredentialsRejected
                                                         : Fault::LinkLost;
}

// Records one piece of candidate evidence into the step being assembled. Free-standing because it
// reads no state: everything it needs is in front of it.
void note(Progress& progress, const Tag tag, const Outcome outcome) {
    Progress fresh;
    fresh.evidence = true;
    fresh.tag = tag;
    fresh.outcome = outcome;
    merge(progress, fresh);
}

} // namespace

std::string_view log_label(const State state) {
    switch (state) {
    case State::Idle:
        return "idle";

    case State::Attempting:
        return "attempting";

    case State::Associated:
        return "associated";

    case State::Connected:
        return "connected";

    case State::Retiring:
        return "retiring";

    case State::Backoff:
        return "backoff";

    case State::GaveUp:
        return "gave-up";

    case State::Stalled:
        return "stalled";
    }

    return "idle";
}

std::string_view error_text(const Fault fault) {
    switch (fault) {
    case Fault::None:
        return {};

    case Fault::LinkLost:
        return "connection lost";

    case Fault::CredentialsRejected:
        return "network rejected the stored credentials";

    // ⛔ IT NAMES THE DRIVER, NOT THE USER'S NETWORK. Blaming the credential for a driver that
    // never answered would send the user to retype a password that was never tested.
    case Fault::DriverUnresponsive:
        return "wi-fi driver did not respond";
    }

    return {};
}

app_model::NetworkFault published_fault(const Fault fault) {
    switch (fault) {
    case Fault::None:
        return app_model::NetworkFault::None;

    case Fault::LinkLost:
        return app_model::NetworkFault::LinkLost;

    case Fault::CredentialsRejected:
        return app_model::NetworkFault::CredentialsRejected;

    // ⛔ THE ONE THAT RAISES A DEVICE FAULT. The reducer turns this into
    // DeviceFaultReason::WifiUnavailable and UIAD 001 §12.1 puts it above everything else.
    case Fault::DriverUnresponsive:
        return app_model::NetworkFault::DriverUnresponsive;
    }

    return app_model::NetworkFault::None;
}

bool operator==(const Truth& lhs, const Truth& rhs) {
    return lhs.phase == rhs.phase && lhs.ipv4 == rhs.ipv4 && lhs.fault == rhs.fault;
}

bool operator!=(const Truth& lhs, const Truth& rhs) {
    return !(lhs == rhs);
}

bool terminal(const Outcome outcome) {
    switch (outcome) {
    case Outcome::Leased:
    case Outcome::LeaseInterrupted:
        return false;

    case Outcome::Unreachable:
    case Outcome::DriverStalled:
    case Outcome::EvidenceLost:
        return true;
    }

    return false;
}

void merge(Progress& into, const Progress& next) {
    into.truth_changed = into.truth_changed || next.truth_changed;

    if (!next.evidence) {
        return;
    }

    if (!into.evidence) {
        into.evidence = true;
        into.tag = next.tag;
        into.outcome = next.outcome;
        return;
    }

    // The first terminal outcome of a tick is the one that stands. See the rule on merge().
    if (terminal(into.outcome)) {
        return;
    }

    into.tag = next.tag;
    into.outcome = next.outcome;
}

// --- Entry points -----------------------------------------------------------

Progress Manager::submit(const Tag tag, const Moment& moment, const Driver& driver) {
    const Truth before = truth_;
    Progress progress;

    // Guarded here as well as in the adapter: a quarantined station issues no further driver
    // operation, and accepting a transaction that can never be judged would be worse than
    // refusing it.
    if (state_ == State::Stalled) {
        return progress;
    }

    // A new submission is the one thing that lifts an exclusive-operation stand-down: by the time
    // one is accepted the operation has released the device, and app_core's barrier is what
    // guarantees that ordering.
    quiesced_ = false;

    // ⬅️ AND IT SUPERSEDES A WITHDRAWAL — checkpoint 3e. A withdrawal says "nothing should be
    // joined"; a submission says "join this". The submission is the newer instruction, and it is
    // also how a restoration is expressed: rejoining a previous pair IS a submit, so leaving the
    // withdrawal set would take the station down again the moment that intent settled.
    withdraw_ = false;

    pending_ = tag;
    advance(progress, moment, driver);

    progress.truth_changed = truth_ != before;
    return progress;
}

Progress Manager::station_absent(const Moment& moment) {
    const Truth before = truth_;
    Progress progress;

    // ⛔ ONE LEGAL STARTING STATE, AND ANYTHING ELSE IS REFUSED WITHOUT TOUCHING MACHINE OR PHYSICAL
    // TRUTH — R6. "No credential, so no station was started" is only true of a machine that has
    // never been given an intent; from anywhere else the honest operation is a disconnect
    // transaction, which is 3e's work.
    const bool absent = state_ == State::Idle && active_ == kNoTag && pending_ == kNoTag && !owed_;

    if (!absent) {
        if (absent_refusals_ != UINT32_MAX) {
            ++absent_refusals_;
        }

        return progress;
    }

    configured_ = false;
    retire_ = false;
    disarm();
    truth_.ipv4 = 0;
    truth_.fault = Fault::None;
    apply(lifecycle_.on(Trigger::CredentialsAbsent), moment);

    progress.truth_changed = truth_ != before;
    return progress;
}

Progress Manager::withdraw(const Moment& moment, const Driver& driver) {
    const Truth before = truth_;
    Progress progress;

    // ⛔ NO DRIVER OPERATION FROM A QUARANTINED STATION, AND NO REFUSAL COUNTED. The stall is
    // already the truth this device is presenting through the typed Device Fault path, and PM
    // ruling 3 keeps the restore vocabulary out of it. The caller reads quarantined() to say why
    // nothing happened rather than inferring it from an unchanged Progress.
    if (state_ == State::Stalled) {
        return progress;
    }

    // An exclusive operation owns the product state — R4. A device erasing itself must not also be
    // taking its station down on somebody else's account; the reset restarts it moments later.
    if (quiesced_) {
        return progress;
    }

    // ⛔ A RETAINED-BUT-UNISSUED INTENT GOES. Its session is over, so joining that network now would
    // be acting on an instruction nobody is waiting for — and the adapter wipes the configuration it
    // was holding for it on the same tick, because credential_wanted() stops naming it.
    pending_ = kNoTag;

    withdraw_ = true;
    advance(progress, moment, driver);

    progress.truth_changed = truth_ != before;
    return progress;
}

Progress Manager::quiesce(const Moment& /*moment*/) {
    const Truth before = truth_;
    Progress progress;

    quiesced_ = true;

    // The retained intent goes: it was never issued, so nothing owes anything for it, and the
    // adapter wipes the configuration it was holding on the same call. An intent that IS in flight
    // keeps its owed completion (invariant 3) and simply gets no successor operation.
    pending_ = kNoTag;
    retire_ = false;

    // ⬅️ AND SO DOES A WITHDRAWAL, ON THE SAME RULE. It is a request for a driver operation, and no
    // driver operation is issued while an exclusive operation holds the device. Resumption after a
    // FAILED erase is the open factory-reset item and is deliberately not modelled here, so the
    // request is forgotten rather than parked.
    withdraw_ = false;

    // ⬅️ THE PUBLISHED TRUTH IS LEFT ALONE ON PURPOSE. The station has not changed physically, and
    // a factory reset already owns the presentation through UIAD 001 §12's priority order. Claiming
    // Disabled here would be a second, contradictory statement about the same radio.
    progress.truth_changed = truth_ != before;
    return progress;
}

Progress Manager::on(const Event& event, const DisconnectCause cause, const Moment& moment,
                     const Driver& driver) {
    const Truth before = truth_;
    Progress progress;

    switch (event.kind) {
    case EventKind::Started:
        // ⬅️ INFORMATIONAL, AND DELIBERATELY SO. submit() establishes the station synchronously
        // through esp_wifi_start(), whose return code is the fact. Waiting for this event instead
        // would hang the one bring-up shape that has already failed on hardware: with the setup
        // access point already running, esp_wifi_set_mode(APSTA) starts the station and posts
        // STA_START from inside the call, and esp_wifi_start() then returns early without posting
        // a second one. That is what StartupArming existed to paper over; not depending on the
        // event at all removes the problem instead of arming around it.
        break;

    case EventKind::Associated:
        // ASSOCIATION IS NOT CONNECTION. §7.4 carries an ipAddress and a station with a link but
        // no lease can reach nothing, so the published phase does not move until GotIp.
        if (owed_ && event.tag == owed_tag_ && state_ == State::Attempting) {
            owed_ = false;
            owed_tag_ = kNoTag;
            state_ = State::Associated;
            arm(moment, policy_.no_lease_ms);
            apply(lifecycle_.on(Trigger::Associated), moment);
        }
        break;

    case EventKind::Disassociated:
        disassociated(progress, event, cause, moment);
        break;

    case EventKind::GotIp:
        if (state_ == State::Associated || state_ == State::Connected) {
            truth_.ipv4 = event.ipv4;
            truth_.fault = Fault::None;
            state_ = State::Connected;

            // A lease cancels the no-lease escalation, and the address is published even when the
            // phase name did not otherwise change — a renewal to a different address is a change
            // the product must not hide.
            disarm();
            apply(lifecycle_.on(Trigger::GotIp), moment);

            if (!superseded()) {
                note(progress, event.tag != kNoTag ? event.tag : active_, Outcome::Leased);
            }
        }
        break;

    case EventKind::LostIp:
        if (state_ == State::Connected) {
            // ⛔ NOT AN ASSOCIATION FAILURE. The ladder is untouched, esp_wifi_connect() is not
            // called while still associated, and the DHCP client is not force-restarted: lwIP has
            // already performed dhcp_release_and_stop(); dhcp_start(); itself on lease expiry
            // (dhcp.c:544-548), and esp_netif_dhcpc_stop() here would interrupt a discovery that
            // is already running, discard the cached offer and schedule another LOST_IP
            // (esp_netif_lwip.c:1566). What changes is the published truth and a bounded interval.
            truth_.ipv4 = 0;
            state_ = State::Associated;

            // ⛔ NO SECOND WAIT — PM DECISION, 2026-08-25. ESP-IDF raises IP_EVENT_STA_LOST_IP only
            // after its own ~120 s lost-IP interval (CONFIG_ESP_NETIF_IP_LOST_TIMER_INTERVAL=120),
            // by which time lwIP has already been rediscovering for that whole interval. Adding the
            // no-lease bound on top would have made the total outage roughly 150 s before the
            // association layer was even asked to help. The documented retirement begins now:
            // disconnect → its matching DISCONNECTED → reconnect. Still no forced DHCP restart, and
            // still no second increment of the reconnect ladder for the same physical outage —
            // Trigger::LeaseLost leaves the counters alone and the retirement itself is deliberate.
            disarm();
            retire_ = true;
            apply(lifecycle_.on(Trigger::LeaseLost), moment);

            if (!superseded()) {
                note(progress, event.tag != kNoTag ? event.tag : active_,
                     Outcome::LeaseInterrupted);
            }
        }
        // Otherwise this is the delayed LOST_IP that follows an ordinary DISCONNECTED — ESP-IDF
        // raises it 120 s later, CONFIG_ESP_NETIF_IP_LOST_TIMER_INTERVAL=120 — and the outage it
        // describes has already been counted once.
        break;

    case EventKind::Stopped:
        stopped(progress, moment);
        break;
    }

    advance(progress, moment, driver);
    progress.truth_changed = truth_ != before;
    return progress;
}

Progress Manager::evidence_lost() {
    const Truth before = truth_;
    Progress progress;

    // ⛔ DETECTED LOSS IS A FAULT, NOT A CASE. The transaction's evidence is incomplete, so no
    // verdict about it can be honest — not Succeeded (§14.3 step 9) and not CredentialsRejected,
    // which would blame the user's password for our lost record.
    const Tag settled = victim();

    if (state_ != State::Stalled) {
        quarantine(Fault::DriverUnresponsive);
    }

    note(progress, settled, Outcome::EvidenceLost);

    progress.truth_changed = truth_ != before;
    return progress;
}

Progress Manager::tick(const Moment& moment, const Driver& driver) {
    const Truth before = truth_;
    Progress progress;

    // ⛔ A STOOD-DOWN STATION HAS NOTHING TO TIME — R4. While an exclusive operation owns the
    // product state no operation is issued, so no deadline can be meaningfully late: quarantining
    // here would manufacture a verdict for a transaction the barrier has already ended, on a device
    // that restarts moments later. An operation still in flight keeps owing its completion
    // (invariant 3), which is untouched by not looking at the clock.
    if (!quiesced_ && due(moment.now_ms)) {
        switch (state_) {
        case State::Attempting:
        case State::Retiring: {
            // ⛔ THE DEADLINE MOVES THE STATE AND CLEARS NOTHING. owed_ stays true, so the late
            // completion can still discharge it — and, arriving in Stalled, cannot revive the
            // transaction it belonged to.
            // R2's victim rule: a retained intent is the transaction with a listener, and the
            // operation that owed this completion is only the CAUSE of its impossibility.
            const Tag settled = victim();
            quarantine(Fault::DriverUnresponsive);
            note(progress, settled, Outcome::DriverStalled);
            break;
        }

        case State::Associated:
            // The bounded no-lease interval expired while still associated. Escalation is the
            // ordinary associated-state retirement — disconnect, its matching DISCONNECTED, then
            // reconnect — because that disconnect has a documented completion and a DHCP poke does
            // not. §11.3's ladder is NOT advanced: this is the same physical outage LostIp already
            // reported, and counting it twice is what the LostIp rules forbid.
            disarm();
            retire_ = true;
            break;

        case State::Backoff:
            // advance() consumes this.
            break;

        case State::Idle:
        case State::Connected:
        case State::GaveUp:
        case State::Stalled:
            disarm();
            break;
        }
    }

    advance(progress, moment, driver);
    progress.truth_changed = truth_ != before;
    return progress;
}

// --- The one place a driver operation is started ----------------------------

void Manager::advance(Progress& progress, const Moment& moment, const Driver& driver) {
    // No further station driver operation, by owner ruling.
    if (state_ == State::Stalled) {
        return;
    }

    // An exclusive operation owns the product state — R4. A device that is erasing itself must not
    // also be joining a network.
    if (quiesced_) {
        return;
    }

    // Invariants 1 and 2: at most one outstanding operation, and nothing may be issued while one
    // is owed. This single return is what makes both structural.
    if (owed_) {
        return;
    }

    // ⬅️ THREE REQUESTS, ONE RULE ABOUT AN EXISTING ASSOCIATION — and checkpoint 3e is what made the
    // third one exist. A newer intent, a pending retirement and a withdrawal all mean "the station
    // must not stay where it is", and for all three the first step from Connected or Associated is
    // the SAME disconnect: invariants 6 and 7, because esp_wifi_set_config() documents
    // ESP_ERR_WIFI_STATE for a station that is still connecting and esp_wifi_connect()'s
    // @attention 2 requires an explicit disconnect when connected. Only what happens AFTER that
    // completion differs, which is why the shared step is issue_retirement() and the difference is
    // the two branches below it.
    if (withdraw_ || pending_ != kNoTag || retire_) {
        if (state_ == State::Connected || state_ == State::Associated) {
            (void)issue_retirement(progress, moment, driver);
            return;
        }

        retire_ = false;

        // ⛔ THE WITHDRAWAL IS ANSWERED FIRST, AND IT NEEDS NO DRIVER OPERATION FROM HERE. Nothing is
        // associated, so there is nothing to disconnect and no completion to wait for; Backoff and
        // GaveUp arrive here too, both waiting to try a credential nobody wants any more. Attempting
        // and Retiring cannot arrive at all — both owe a completion, and the guard above returned.
        if (withdraw_) {
            withdrawn(moment);
            return;
        }

        // Invariant 5: from a non-associated state the newest intent is simply adopted.
        if (pending_ != kNoTag) {
            adopt(moment);
        }
    }

    if (active_ == kNoTag) {
        return;
    }

    if (state_ == State::Backoff) {
        if (!due(moment.now_ms)) {
            return;
        }

        state_ = State::Idle;
        disarm();
    }

    // Associated and Connected are waiting on the network rather than on us; GaveUp is §16.2's
    // terminal state and only a configuration change may re-arm it.
    if (state_ != State::Idle) {
        return;
    }

    if (!configured_) {
        if (driver.configure == nullptr || !driver.configure(driver.context, active_)) {
            refused(progress);
            return;
        }

        // ⛔ NOTHING IS OWED BY A set_config. It is synchronous and posts no event, so its return
        // code is the whole completion (invariant 8).
        configured_ = true;
    }

    apply(lifecycle_.on(Trigger::Associating), moment);
    state_ = State::Attempting;
    arm(moment, policy_.completion_ms);

    if (driver.connect == nullptr || !driver.connect(driver.context, active_)) {
        refused(progress);
        return;
    }

    owed_ = true;
    owed_tag_ = active_;
}

// --- Internals --------------------------------------------------------------

bool Manager::issue_retirement(Progress& progress, const Moment& moment, const Driver& driver) {
    // ⛔ THE ONE DISCONNECT THE PINNED SDK DOCUMENTS A COMPLETION FOR, ISSUED FROM THE ONE STATE PAIR
    // IT DOCUMENTS IT IN. Extracted because three callers need exactly this step and nothing else,
    // and a second copy of it is a second place invariants 1, 2 and 8 could be got wrong. It is
    // reached only from advance(), which is what keeps "one place issues a driver operation" true.
    //
    // Charged to `active_` — the identity whose association is being taken down — so every
    // completion the call provokes is stamped with the operation that provoked it (§8.1).
    retire_ = false;
    state_ = State::Retiring;
    arm(moment, policy_.completion_ms);

    if (driver.disconnect == nullptr || !driver.disconnect(driver.context, active_)) {
        // Invariant 8: a refused synchronous call creates no owed completion, and it quarantines.
        refused(progress);
        return false;
    }

    owed_ = true;
    owed_tag_ = active_;
    return true;
}

void Manager::disassociated(Progress& progress, const Event& event, const DisconnectCause cause,
                            const Moment& moment) {
    const bool matches = owed_ && event.tag == owed_tag_;

    if (matches) {
        // Invariant 3: the ONLY thing that clears an owed completion.
        owed_ = false;
        owed_tag_ = kNoTag;
    }

    if (state_ == State::Stalled) {
        // ⛔ A LATE MATCHING COMPLETION CLEARS THE OWED FACT AND NOTHING ELSE. It does not resume,
        // retry or validate the transaction it belonged to; recovery still requires an explicit
        // user restart.
        return;
    }

    if (matches && state_ == State::Retiring) {
        // The association was retired on purpose. Not a failure, so §11.3's ladder is untouched
        // and no candidate evidence is produced — the intent that asked for the retirement is what
        // will be judged.
        state_ = State::Idle;
        disarm();
        truth_.ipv4 = 0;
        return;
    }

    if (!matches && state_ != State::Associated && state_ != State::Connected) {
        // A completion for an operation nobody is waiting on, with no association to lose. It
        // cannot revive a settled transaction and it changes no published truth.
        return;
    }

    // Either the attempt we were waiting on failed, or an association we were not retiring went
    // away. Both are §16.1 Wi-Fi errors; §16.2 and §11.3 decide what happens next.
    truth_.ipv4 = 0;
    truth_.fault = fault_for(cause);
    state_ = State::Idle;
    disarm();
    apply(lifecycle_.on(Trigger::Disconnected, cause), moment);

    // ⛔ A SUPERSEDED ATTEMPT'S FAILURE IS NOT A VERDICT — R2. It is the step that lets advance()
    // adopt the intent that replaced it, and the transaction with a listener is that successor. If
    // this were published it would take the tick's single evidence result and, being terminal
    // whenever the ladder saturates or §16.2 gives up, would survive merge() and suppress the
    // successor's own outcome in the same tick.
    if (superseded()) {
        return;
    }

    // ⬅️ THE STATION STOPPING IS WHAT MAKES THIS TERMINAL FOR A CANDIDATE, not the reason code.
    // §16.2's give-up and §11.3's saturated ladder both publish Unavailable, and both mean the
    // station is no longer going to reach that network on its own within a useful time.
    const Outcome outcome = truth_.phase == NetworkPhase::Unavailable ? Outcome::Unreachable
                                                                      : Outcome::LeaseInterrupted;

    note(progress, event.tag != kNoTag ? event.tag : active_, outcome);
}

void Manager::stopped(Progress& progress, const Moment& moment) {
    // The station interface went away underneath us.
    //
    // ⛔ WHY AN INTERFACE-WIDE STOP MAY END AN OWED OPERATION WITHOUT BEING ITS COMPLETION — R5, and
    // this is the one place invariant 3 is deliberately not the whole story. Invariant 3 says a
    // completion is cleared only by consuming the matching documented terminal event, and that rule
    // exists to stop a DEADLINE or a guess standing in for the driver's answer. WIFI_EVENT_STA_STOP
    // is neither: it is the driver's own statement that the station interface no longer exists, and
    // an operation issued against an interface that is gone can never be completed by anything. So
    // the owed fact is discharged — but it is discharged as UNANSWERED, not as satisfied, which is
    // why the candidate is settled below rather than allowed to proceed. No per-tag match is applied
    // for the same reason: a STOP is not the completion of one operation, it is the end of all of
    // them, and demanding the tag match would leave an owed fact that nothing could ever clear.
    //
    // ⬅️ THE INTENT IS FORGOTTEN RATHER THAN RE-ESTABLISHED, which is the committed behaviour and is
    // preserved deliberately: the adapter's copy of the credential is wiped as soon as set_config
    // consumes it, so there would be nothing left to reconfigure with. A new submission is what
    // starts a station again — and the adapter's running() is corrected to false so that submission
    // performs the bring-up instead of skipping it. Deliberate stop-and-resume semantics belong to
    // 3e; this path is what makes an UNEXPECTED stop truthful.
    const Tag settled = victim();

    owed_ = false;
    owed_tag_ = kNoTag;
    configured_ = false;
    active_ = kNoTag;
    pending_ = kNoTag;
    retire_ = false;

    if (state_ == State::Stalled) {
        return;
    }

    const bool had_intent = settled != kNoTag;

    state_ = State::Idle;
    disarm();
    truth_.ipv4 = 0;
    truth_.fault = Fault::None;
    apply(lifecycle_.on(Trigger::Stopped), moment);

    // ⛔ A LIVE CANDIDATE IS TERMINATED HERE RATHER THAN LEFT TO TIME OUT — R5. Its operation was
    // owed a completion that can no longer arrive, or its intent was never issued at all; either
    // way the station cannot answer for it. Left alone it would reach the browser as the generic
    // WifiTimedOut thirty seconds later, which would blame the user's network for an interface that
    // disappeared. Reported under R2's victim identity, so the transaction that is actually being
    // waited on is the one that learns.
    if (had_intent) {
        note(progress, settled, Outcome::EvidenceLost);
    }
}

void Manager::adopt(const Moment& moment) {
    active_ = pending_;
    pending_ = kNoTag;
    configured_ = false;

    // A NEW CREDENTIAL SET. §16.2's give-up is terminal on purpose and only "a configuration
    // change" may re-arm it; a new submission is that change. Nothing carries over, because none
    // of it describes the credential now being tried.
    lifecycle_.reset();
    state_ = State::Idle;
    disarm();
    truth_.ipv4 = 0;
    truth_.fault = Fault::None;

    apply(lifecycle_.on(Trigger::Started), moment);
}

void Manager::withdrawn(const Moment& moment) {
    // ⛔ THE INTENT IS FORGOTTEN, AND THAT IS WHAT MAKES THE ADAPTER WIPE ITS HELD CREDENTIAL.
    // credential_wanted() names nothing once there is neither a retained intent nor an unconfigured
    // active one, and service() consults it on every step — so the passphrase the abandoned
    // candidate's configuration was built from goes on the tick the withdrawal completes, not at
    // some later boundary that could be forgotten.
    withdraw_ = false;
    active_ = kNoTag;
    pending_ = kNoTag;
    configured_ = false;
    retire_ = false;

    state_ = State::Idle;
    disarm();
    truth_.ipv4 = 0;
    truth_.fault = Fault::None;

    // ⬅️ THE SAME TRANSITION AN UN-PROVISIONED BOOT TAKES, DELIBERATELY. It publishes Disabled and
    // clears the retry counters, which is exactly the truth about a station that has been taken
    // down and holds no credential to try. No phase is invented for it: §7.4 defines six, and "the
    // station is not running" is Disabled.
    //
    // ⛔ AND NO CANDIDATE EVIDENCE IS PRODUCED. A withdrawal is not a transaction anybody is
    // waiting on a verdict for — the session that submitted the candidate is already over, which is
    // why the withdrawal was asked for. Publishing an outcome here would occupy the tick's single
    // evidence slot on behalf of a transaction with no listener.
    apply(lifecycle_.on(Trigger::CredentialsAbsent), moment);
}

void Manager::apply(const network::Step& step, const Moment& moment) {
    truth_.phase = step.phase;

    switch (step.action) {
    case NextAction::None:
    case NextAction::Attempt:
        // advance() decides when to issue; the lifecycle decides what the product says and how
        // long until the next attempt is due.
        break;

    case NextAction::WaitBackoff: {
        // §11.3's ladder, jittered from the byte the caller supplied. Deadline-based and never a
        // blocking wait: the application task compares it on its own bounded tick (invariant 9).
        const auto delay = delay_for(step.retry_attempt, moment.jitter);
        state_ = State::Backoff;
        arm(moment, static_cast<std::uint32_t>(delay.count()));
        break;
    }

    case NextAction::GiveUp:
        // §16.2: "do not continuously retry at high frequency: ... invalid configuration".
        state_ = State::GaveUp;
        disarm();
        break;
    }
}

Tag Manager::victim() const {
    if (pending_ != kNoTag) {
        return pending_;
    }

    if (owed_tag_ != kNoTag) {
        return owed_tag_;
    }

    return active_;
}

void Manager::refused(Progress& progress) {
    // ⛔ A REFUSED SYNCHRONOUS CALL CREATES NO OWED COMPLETION (invariant 8), AND IT QUARANTINES.
    // Each operation is issued only in the state where the pinned SDK documents it to succeed —
    // set_config from a station that is neither connected nor connecting, connect from a
    // disconnected station, disconnect from a connected one. A refusal therefore contradicts the
    // model this manager is built on, and the owner's ruling for a driver that is not behaving as
    // documented is a truthful quarantine with an explicit user restart, not a retry loop against
    // a driver we no longer understand. It also converges immediately, which a bounded retry of an
    // unexplained refusal would not.
    //
    // ⬅️ THE VICTIM IS READ FIRST — R2. quarantine() clears `pending_`, and the retained intent is
    // exactly the transaction whose browser is waiting.
    const Tag settled = victim();
    quarantine(Fault::DriverUnresponsive);
    note(progress, settled, Outcome::DriverStalled);
}

void Manager::quarantine(const Fault fault) {
    state_ = State::Stalled;
    disarm();
    retire_ = false;
    pending_ = kNoTag;

    truth_.phase = NetworkPhase::Unavailable;
    truth_.ipv4 = 0;
    truth_.fault = fault;

    // ⛔ owed_ IS NOT CLEARED — invariant 3. Only the matching terminal event may clear it, and
    // arriving here it will change nothing else.
}

void Manager::arm(const Moment& moment, const std::uint32_t span_ms) {
    armed_ = true;
    armed_at_ms_ = moment.now_ms;
    span_ms_ = span_ms;
}

void Manager::disarm() {
    armed_ = false;
    armed_at_ms_ = 0;
    span_ms_ = 0;
}

bool Manager::due(const std::uint32_t now_ms) const {
    return armed_ && elapsed_since(armed_at_ms_, now_ms) >= span_ms_;
}

} // namespace prd::network::station
