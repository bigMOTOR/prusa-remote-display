// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
//
// Two browser steps, one persistence transaction. Step 1 proves the Wi-Fi and stores
// nothing; step 2 collects the printer and is what triggers the save.
//
// Every value is written with textContent, never innerHTML. Keep it that way: innerHTML
// would reintroduce an injection path that this page does not otherwise have.
//
// ⛔ NOTHING THE USER TYPED IS EVER SHOWN BACK. The server reports a field name and a rule;
// the messages below are fixed text chosen by that rule. No submitted byte is echoed.

(function () {
  "use strict";

  var token = "";
  var polling = null;

  // Set when the device is observed to have restarted mid-attempt. See readStatus().
  var deviceRestarted = false;

  // A submission has been accepted and no terminal state has been seen yet.
  var awaiting = false;

  // Whether this connection may change the display's configuration.
  //
  // ⬅️ THE DISPLAY DECIDES, NOT THE PAGE. The device reports it per request, from the network
  // interface the connection actually arrived on — reached over the local network instead of
  // the display's own setup network, every configuration change is refused. The page's job is
  // only to stop offering controls that would be refused: the same rule that kept a Save
  // action off this page while nothing could be saved.
  var canConfigure = true;
  var canInstall = false;
  var updateVersion = "";
  var updateInstalling = false;

  // An outcome is still pending — from the POST itself OR from the validation that follows it.
  //
  // ⬅️ THE WINDOW IS THE ONE THE BUTTON IS DISABLED FOR, not merely the POST. Releasing it when
  // the POST returned still let a second press through while the station was validating, and on
  // the device a second Wi-Fi submission RESTARTS the station and abandons the attempt already
  // running — one stray tap turning into "the first attempt failed". A deliberate resubmission
  // after a failure is still honoured: the flag clears the moment the page is interactive again.
  //
  // ⛔ DISABLING THE BUTTON IS NOT ENOUGH, and the browser suite proved it: a disabled button
  // stops a tap, but a `submit` event still reaches the handler — pressing Enter in a field
  // does it, and so does a second tap landing before the first press disables anything. Three
  // POSTs went out where one was intended. On the Wi-Fi step each duplicate restarts the
  // station and abandons the attempt already running, which is exactly the shape of "the first
  // attempt failed and the second succeeded".
  var inFlight = false;

  var WIFI_FIELDS = ["wifiSsid", "wifiPassword"];
  var PRINTER_FIELDS = ["printerHost", "printerPort", "printerApiKey"];

  // Field-specific, non-secret, and phrased as what to do rather than what went wrong.
  var FIELD_MESSAGES = {
    wifiSsid: {
      missing: "Enter your network name.",
      "too-long": "That network name is too long.",
      "bad-character": "That network name contains characters the display cannot use.",
    },
    wifiPassword: {
      "too-short": "A Wi-Fi password needs at least 8 characters.",
      "too-long": "That password is too long.",
      "bad-character": "That password contains characters the display cannot use.",
    },
    printerHost: {
      missing: "Enter your printer's address.",
      "too-long": "That address is too long.",
      "bad-character":
        "Enter just the address, with no http:// and no port — for example 192.168.1.50",
    },
    printerPort: {
      missing: "Enter the port. It is usually 80.",
      "too-short": "That port is not valid.",
      "too-long": "That port is not valid.",
      "bad-character": "The port must be digits only.",
    },
    printerApiKey: {
      missing: "Enter your printer's API key.",
      "too-long": "That API key is too long.",
      "bad-character": "That API key contains characters the display cannot use.",
    },
  };

  function byId(id) {
    return document.getElementById(id);
  }

  // Reads a field, with surrounding whitespace removed.
  //
  // ⬅️ WHITESPACE IS NOT CONTENT, AND REJECTING IT FAILED THE PRINTER STEP. A phone that
  // pastes or autofills an address, a port or an API key commonly appends a space or a
  // newline. The address grammar permits neither, the port accepts digits only, and the key
  // must be printable — so ONE paste could fail all three fields at once, and printerHost's
  // explanation ("no http:// and no port") described a fault the value did not have.
  //
  // Applied to the printer fields ONLY. A Wi-Fi SSID and a WPA2 passphrase may legitimately
  // begin or end with a space, so trimming those would refuse a network the user really has.
  // The trimmed value is written back, so what was validated is what the field shows.
  function normalised(id) {
    var input = byId(id);
    if (!input) {
      return "";
    }

    var value = input.value.trim();
    if (value !== input.value) {
      input.value = value;
    }

    return value;
  }

  function setText(id, value) {
    var node = byId(id);
    if (node) {
      node.textContent = value;
    }
  }

  function show(node, visible) {
    if (node) {
      node.hidden = !visible;
    }
  }

  function message(field, outcome) {
    var forField = FIELD_MESSAGES[field] || {};
    return forField[outcome] || "Check this field.";
  }

  // Clears one field's error state. Called both before a submit and as the user edits, so a
  // corrected field stops being marked immediately rather than staying flagged until the
  // next submit — which is how the physical validation screenshot came to show three marked
  // printer fields when only one was actually wrong.
  function clearField(field) {
    var input = byId(field);
    if (input) {
      input.classList.remove("invalid");
      input.removeAttribute("aria-invalid");
    }
    var error = byId(field + "Error");
    if (error) {
      error.textContent = "";
      show(error, false);
    }
  }

  // Marks exactly the fields the server reported as failing, and nothing else.
  function applyReport(fields, report, summaryId) {
    var firstInvalid = null;
    var count = 0;

    fields.forEach(function (field) {
      clearField(field);

      var outcome = report ? report[field] : undefined;
      if (!outcome || outcome === "ok") {
        return;
      }

      count += 1;

      var input = byId(field);
      if (input) {
        input.classList.add("invalid");
        input.setAttribute("aria-invalid", "true");
        if (firstInvalid === null) {
          firstInvalid = input;
        }
      }

      var error = byId(field + "Error");
      if (error) {
        error.textContent = message(field, outcome);
        show(error, true);
      }
    });

    var summary = byId(summaryId);
    if (summary && count > 0) {
      // ⛔ THE CLASS IS ASSIGNED, NOT ADDED. Step 1's confirmation styles this same element
      // as a success, and only writing textContent left "3 fields need attention." wearing
      // the success accent. Validation failure is an error and must read as one.
      summary.className = "summary bad";
      summary.textContent =
        count === 1 ? "One field needs attention." : count + " fields need attention.";
      show(summary, true);
    } else if (summary) {
      show(summary, false);
    }

    // The summary sits above the button, and the first offending field is scrolled to and
    // focused — so a phone user is taken to the problem instead of left at the bottom of
    // the page with a line of text below the action they just pressed.
    if (firstInvalid !== null) {
      firstInvalid.scrollIntoView({ block: "center" });
      firstInvalid.focus();
    }
  }

  function clearAll(fields, summaryId) {
    fields.forEach(clearField);
    show(byId(summaryId), false);
  }

  // ⛔ THE ONE RESPONSE applyReport() CANNOT SPEAK FOR, AND IT SHOWED THE USER NOTHING AT ALL.
  //
  // A validated submission the device could not hand to its application task answers 503 with the
  // fixed body {"accepted":false,"error":"unavailable"}. That parses, so the promise RESOLVES and
  // .catch() never runs; the submit handler took the !result.ok branch and called applyReport(),
  // which found no per-field outcomes, counted zero invalid fields and therefore HID the summary.
  // The button came back and the page said nothing. A truthful recoverable failure has to be
  // visible, so this is the one case that gets its own answer.
  //
  // ⬅️ AND IT IS KEYED ON THE FIXED `error` VALUE, NOT ON THE STATUS CODE. The gate's own refusals
  // send {"accepted":false} with no `error` member and keep their existing per-field semantics —
  // turning every refusal into this message would replace real field guidance with a generic line.
  function unavailable(result) {
    return Boolean(
      result && !result.ok && result.report && result.report.error === "unavailable",
    );
  }

  // ⛔ FIXED, TRUTHFUL, AND IT DOES NOT SAY "THE DISPLAY DID NOT ANSWER" — it did answer, with a
  // 503. It also discloses no internal reason: whether the heap refused or the mailbox was full is
  // not something a person can act on, and "try again" is the whole of the available action.
  // ⛔ COMMENTS IN THIS FILE ARE SHIPPED BYTES. It is an EMBED_FILES asset, so every sentence below
  // is compiled into the firmware image — which is why none of them quotes the removed wording
  // verbatim. A `strings` audit of the image must be able to tell rendered copy from a note about
  // copy, and a comment reproducing the phrase it exists to remove destroys exactly that.
  //
  // ⛔ THE ONE PERSISTENCE ABSOLUTE THAT SURVIVES THIS FILE, AND IT IS BOUNDED BY A SEAM CONTRACT.
  // This is the 503 the server returns when a validated body could NOT be handed to app_core.
  // SubmissionHandoff::Unavailable states the guarantee in the accepted seam: "Nothing was started,
  // nothing was persisted, and a submission already pending — if there was one — is untouched." The
  // refusal happens BEFORE app_core takes ownership, so no transaction began, no record was written
  // and the radio was never touched. Every other persistence claim in this file was removed because
  // no such bound existed behind it; this one is kept because it does.
  var UNAVAILABLE_TEXT = "The display could not start that check. Nothing was saved; try again.";

  // Shows it on the stage's own summary, with the error styling, marking no field.
  function failUnavailable(summaryId) {
    var summary = byId(summaryId);
    if (!summary) {
      return;
    }

    // Assigned, not added: step 1's confirmation styles this same element as a success, and only
    // writing textContent would leave an error wearing the success accent.
    summary.className = "summary bad";
    summary.textContent = UNAVAILABLE_TEXT;
    show(summary, true);
    summary.scrollIntoView({ block: "center" });
  }

  function busy(buttonId, isBusy, label, busyLabel) {
    var button = byId(buttonId);
    if (button) {
      button.disabled = isBusy;
      button.textContent = isBusy ? busyLabel : label;
    }
  }

  function readStatus() {
    return fetch("/api/status", {
      headers: { Accept: "application/json" },
      cache: "no-store",
      credentials: "omit",
    })
      .then(function (response) {
        if (!response.ok) {
          throw new Error("status " + response.status);
        }
        return response.json();
      })
      .then(function (status) {
        if (typeof status.product === "string") {
          setText("product", status.product);
        }
        if (typeof status.version === "string") {
          setText("version", status.version);
        }

        // ⛔ A CHANGED TOKEN MEANS THE DEVICE RESTARTED, AND THAT IS WHY THIS EXISTS. The
        // session token is minted when the server starts, so a different one can only mean the
        // server did. A restarted device reports "editing" — indistinguishable from "has not
        // started yet" — and the page would poll a device that has forgotten the attempt for
        // ever, leaving the button stuck on Checking…. Compared BEFORE the new value is stored.
        //
        // Not a timeout. A timeout would hide this; the token says it outright.
        // Absent counts as configurable so an older device that does not report the field
        // still presents its forms. It cannot make a refusal into an acceptance: the device
        // refuses on its own account, and this only decides what the page offers.
        if (typeof status.canConfigure === "boolean") {
          canConfigure = status.canConfigure;
        }
        if (typeof status.canInstall === "boolean") {
          canInstall = status.canInstall;
        }
        if (typeof status.updateVersion === "string") {
          updateVersion = status.updateVersion;
        }
        if (typeof status.updateInstalling === "boolean") {
          updateInstalling = status.updateInstalling;
        }

        if (typeof status.csrfToken === "string") {
          if (token !== "" && status.csrfToken !== token && awaiting) {
            deviceRestarted = true;
          }
          token = status.csrfToken;
        }
        return status;
      });
  }

  function post(stage, body) {
    body.stage = stage;
    return fetch("/api/setup", {
      method: "POST",
      cache: "no-store",
      credentials: "omit",
      headers: {
        "Content-Type": "application/json",
        Accept: "application/json",
        "X-PRD-Setup-Token": token,
      },
      body: JSON.stringify(body),
    }).then(function (response) {
      return response
        .json()
        .catch(function () {
          return {};
        })
        .then(function (report) {
          return { ok: response.ok, report: report };
        });
    });
  }

  function renderUpdate() {
    var available = updateVersion !== "";
    show(byId("updateCard"), available || updateInstalling);
    setText("updateVersion", updateVersion);
    show(byId("updateInstall"), available && canInstall && !updateInstalling);
    setText(
      "updateStatus",
      updateInstalling
        ? "Installing on the display. Keep it powered."
        : available && !canInstall
          ? "Hold the display button to open its authorized settings network, then install here."
          : "",
    );
  }

  function installUpdate() {
    if (!canInstall || updateInstalling) {
      return;
    }
    busy("updateInstall", true, "Install update", "Starting…");
    fetch("/api/update", {
      method: "POST",
      cache: "no-store",
      credentials: "omit",
      headers: {
        "Content-Type": "application/json",
        Accept: "application/json",
        "X-PRD-Setup-Token": token,
      },
      body: "{}",
    })
      .then(function (response) {
        if (!response.ok) {
          throw new Error("update " + response.status);
        }
        updateInstalling = true;
        canInstall = false;
        renderUpdate();
        startPolling();
      })
      .catch(function () {
        busy("updateInstall", false, "Install update", "Starting…");
        setText("updateStatus", "The display could not start the update. Try again.");
      });
  }

  function enterPrinterStep() {
    inFlight = false;
    show(byId("wifiForm"), false);
    show(byId("printerForm"), true);
    setText("lead", "That Wi-Fi works. Now tell the display about your printer.");
    var summary = byId("printerSummary");
    if (summary) {
      summary.className = "summary good";
      summary.textContent = "Connected to your Wi-Fi.";
      show(summary, true);
    }
  }

  function finish() {
    inFlight = false;
    show(byId("wifiForm"), false);
    show(byId("printerForm"), false);
    setText("lead", "Setup complete.");
    var done = byId("done");
    if (done) {
      done.textContent =
        "Saved. This setup network is switching off — the display is on your own Wi-Fi now.";
      show(done, true);
    }
  }

  function failWifi(reason) {
    inFlight = false;
    busy("wifiSubmit", false, "Check this Wi-Fi", "Checking…");
    show(byId("wifiForm"), true);
    show(byId("printerForm"), false);

    var summary = byId("wifiSummary");
    if (summary) {
      // A failed attempt is an error too, and was previously drawn with the generic accent.
      summary.className = "summary bad";
      summary.textContent = reasonText(reason);
      show(summary, true);
      summary.scrollIntoView({ block: "center" });
    }
  }

  function reasonText(reason) {
    if (reason === "wifi-unreachable") {
      return "The display could not join that network. Check the name and the password.";
    }
    if (reason === "wifi-timed-out") {
      return "That network did not finish connecting. Check the password, or move the display closer to the router.";
    }
    // ⛔ THE PAGE CANNOT KNOW WHERE THE RESTART LANDED, SO IT MAY NOT SAY. This reason is the page's
    // own detection of a session token that changed under it; the restart could have happened before
    // anything was written, between the two candidate writes, or after an activation pointer had
    // moved. The blanket reassurance this replaces was a guess dressed as a guarantee. What is
    // certainly true is that setup was interrupted, and the one useful action is to look at what the
    // display is now showing — it will say whether it is configured.
    if (reason === "device-restarted") {
      return "Setup was interrupted when the display restarted. Check what the display is showing before trying again.";
    }
    // ⛔ THE OWNER'S DriverStalled RULING, IN THE ONE PLACE THE USER CAN READ IT. The Wi-Fi driver
    // accepted an operation and never reported the completion it owes, or the record of that
    // completion was lost. The display has quarantined its station: it will issue no further
    // driver operation and only a restart clears that, so "try again" would be false — the retry
    // could not be judged either. It deliberately does NOT say the network or the password was
    // wrong, because neither was tested.
    if (reason === "wifi-driver-stalled" || reason === "wifi-evidence-lost") {
      return "Wi-Fi unavailable. Restart the display.";
    }
    // ⛔ CHECKPOINT 3e's RESTORE FAILURE, AND IT IS ABOUT THE OLD NETWORK RATHER THAN THE NEW ONE.
    // The attempt was abandoned, the display tried to rejoin the network it had been using, and that
    // network did not come back. Its Wi-Fi is truthfully reported as unavailable; nothing takes the
    // station down on account of this outcome, and this sentence must not imply that it did.
    //
    // ⛔ THE ONLY GUARANTEE THIS MAY MAKE IS THE NARROW ONE: THE SAVED ACTIVE CONFIGURATION WAS NOT
    // REPLACED. It used to assert that nothing had reached flash, which is not true of every route
    // that reaches here — when the printer stage had been submitted, both candidate records were
    // already written to and verified in the INACTIVE flash slots. What is true on
    // every route is that no activation pointer moved, so the configuration the display was running
    // is still the one it will run.
    //
    // It must not read like the generic fallback: "try again" would invite a retry of something that
    // was never the problem, and this condition has exactly one action behind it — enter Wi-Fi
    // details. It must also not blame the credential the user has just typed: the network it failed
    // to rejoin was one the display had stored and previously proven, so nothing they retype
    // affects it.
    if (reason === "wifi-restore-failed") {
      return "The display could not rejoin your previous network. Your saved configuration was not replaced; enter your Wi-Fi details again.";
    }
    // ⛔ AND THIS ONE IS NOT ABOUT ANY NETWORK AT ALL. The display could not work out which saved
    // network it had been running, so it never tried to rejoin one.
    //
    // ⛔ THREE ROUTES SHARE IT AND THEY LEAVE THE DEVICE IN DIFFERENT STATES, WHICH IS WHY THE
    // SENTENCE MAY ONLY MAKE THE NARROW CLAIM. It used to assert that the device was untouched, which
    // is true of only the first of them:
    //   - refused BEFORE the radio was touched: nothing written, nothing joined;
    //   - refused because the two accounts of the previous pair disagreed: candidate records had
    //     already been written to the inactive slots AND the radio had already joined the candidate;
    //   - refused because the retained record would not load after the radio had been disturbed: the
    //     radio had joined the candidate and is being taken down.
    // What holds on all three is that no activation pointer moved, so the saved configuration was not
    // replaced — and that is exactly what the sentence says and no more.
    //
    // It must not read like the restore failure above, which reports an attempt that WAS made against
    // the network, and it must not invite a bare retry of a submission that would be refused again
    // for the same reason. The one action that can change the outcome is a restart.
    if (reason === "wifi-rollback-unavailable") {
      return "The display could not verify its saved network. Your saved configuration was not replaced; restart the display and try again.";
    }
    // ⛔ THREE STORAGE REASONS SHARE THIS BRANCH AND ONLY ONE OF THEM WROTE NOTHING.
    // `storage-rejected` is §13.3 step 1 refusing the record before any write; but
    // `storage-candidate-failed` means a candidate write was attempted and did not read back, and
    // `storage-commit-failed` means the candidate WAS written and verified and only the pointer
    // failed to move. So an untouched-device claim is false for two of the three. What holds for all
    // three is that no activation pointer moved — the configuration the display was running is still
    // the one it will run — and the retry action is unchanged.
    if (reason && reason.indexOf("storage") === 0) {
      return "The display could not save that configuration. Your saved configuration was not replaced; try again.";
    }
    // ⛔ AN INTERRUPTED ACTIVATION IS THE ONE FAILURE THAT MAY HAVE MOVED A POINTER, so it gets its
    // own branch rather than the fallback's blanket reassurance. `promotion-failed` means the
    // activation boundary was entered: the recovery record is in flash and a pointer may already
    // have moved, which the next boot resolves. The page must therefore NOT claim the previous
    // configuration is unchanged — it may not be — and the only action that lets the display finish
    // what it started is a restart.
    if (reason === "promotion-failed") {
      return "The display could not finish saving the configuration. Restart the display before trying setup again.";
    }
    // Printer failures end the transaction after Wi-Fi was validated. The server requires
    // another Wi-Fi check before printer retry; retained fields are shown again afterwards.
    // Candidate records may already have been written: do not promise untouched flash.
    if (reason === "printer-unreachable") {
      return "That Wi-Fi worked, but the printer did not answer. Check its power, address and port. Check this Wi-Fi again, then correct the printer details.";
    }
    if (reason === "printer-unauthorized") {
      return "That Wi-Fi worked, but the printer rejected the API key. Check the PrusaLink API key on the printer. Check this Wi-Fi again, then correct the key.";
    }
    if (reason === "printer-unsupported") {
      return "That Wi-Fi worked, but the printer interface is not supported. Check that the address is for a supported PrusaLink printer. Check this Wi-Fi again before retrying.";
    }
    if (reason === "printer-unusable-response") {
      return "That Wi-Fi worked, but the printer response could not be used. Check the PrusaLink address and printer firmware. Check this Wi-Fi again before retrying.";
    }
    // An unknown reason cannot safely make any persistence guarantee.
    return "The display could not complete that request. Check the details you entered and try again.";
  }

  function render(status) {
    renderUpdate();
    // ⬅️ CHECKED FIRST, AND IT ENDS THE RENDER. On a connection that cannot configure the
    // display there is no flow to report: the forms go away, the notice explains why, and
    // there is nothing further to poll for. Returning false stops the timer.
    if (!canConfigure) {
      show(byId("wifiForm"), false);
      show(byId("printerForm"), false);
      show(byId("done"), false);
      show(byId("readOnly"), true);
      setText("lead", "Connected to your network.");
      return false;
    }

    // Checked before the state, because a restarted device's state describes a different
    // attempt from the one being waited on.
    if (deviceRestarted) {
      deviceRestarted = false;
      awaiting = false;
      busy("printerSubmit", false, "Save and finish", "Saving…");
      failWifi("device-restarted");
      return false;
    }

    switch (status.setupState) {
      case "validating":
        busy("wifiSubmit", true, "Check this Wi-Fi", "Checking…");
        return true;

      case "wifi-validated":
        busy("wifiSubmit", false, "Check this Wi-Fi", "Checking…");
        awaiting = false;
        enterPrinterStep();
        return false;

      case "succeeded":
        awaiting = false;
        finish();
        return false;

      case "failed":
        awaiting = false;
        busy("printerSubmit", false, "Save and finish", "Saving…");
        failWifi(status.setupReason);
        return false;

      default:
        // ⬅️ "editing" IS NOT TERMINAL WHILE A SUBMISSION IS IN FLIGHT, and treating it as
        // terminal was a real race: the device publishes Validating on the application
        // task, up to one tick after it takes the submission off the queue, so a poll that
        // lands in that window would otherwise stop the timer and the page would never
        // learn the outcome. Found by driving the page against a stubbed status route.
        return awaiting;
    }
  }

  function poll() {
    readStatus()
      .then(function (status) {
        var keepPolling = render(status);
        if (!keepPolling && polling !== null) {
          clearInterval(polling);
          polling = null;
        }
      })
      .catch(function () {
        // Expected once the access point has stopped after success: the last rendered
        // message stands, which is why it is shown before the shutdown rather than fetched
        // after it.
      });
  }

  function startPolling() {
    awaiting = true;
    if (polling === null) {
      polling = setInterval(poll, 1000);
    }
  }

  function submitWifi(event) {
    event.preventDefault();
    if (inFlight) {
      return;
    }
    inFlight = true;
    clearAll(WIFI_FIELDS, "wifiSummary");
    busy("wifiSubmit", true, "Check this Wi-Fi", "Checking…");

    post("wifi", {
      wifiSsid: byId("wifiSsid").value,
      wifiPassword: byId("wifiPassword").value,
    })
      .then(function (result) {
        // Before the report branch: this response carries no per-field outcomes, so applyReport()
        // would hide the summary and leave the user with no error at all. No polling is started —
        // nothing was handed over, so no validation was ever going to begin.
        if (unavailable(result)) {
          inFlight = false;
          busy("wifiSubmit", false, "Check this Wi-Fi", "Checking…");
          failUnavailable("wifiSummary");
          return;
        }
        if (!result.ok || !result.report.accepted) {
          inFlight = false;
          busy("wifiSubmit", false, "Check this Wi-Fi", "Checking…");
          applyReport(WIFI_FIELDS, result.report, "wifiSummary");
          return;
        }
        startPolling();
      })
      .catch(function () {
        inFlight = false;
        busy("wifiSubmit", false, "Check this Wi-Fi", "Checking…");
        setText("wifiSummary", "The display did not answer. Try again.");
        show(byId("wifiSummary"), true);
      });
  }

  function submitPrinter(event) {
    event.preventDefault();
    if (inFlight) {
      return;
    }
    inFlight = true;
    clearAll(PRINTER_FIELDS, "printerSummary");
    busy("printerSubmit", true, "Save and finish", "Saving…");

    post("printer", {
      printerHost: normalised("printerHost"),
      printerPort: normalised("printerPort"),
      printerApiKey: normalised("printerApiKey"),
    })
      .then(function (result) {
        // Same rule as step 1, and it must be the same rule: a person who reaches step 2 and is
        // told nothing has no way to know whether their printer details were even read.
        if (unavailable(result)) {
          inFlight = false;
          busy("printerSubmit", false, "Save and finish", "Saving…");
          failUnavailable("printerSummary");
          return;
        }
        if (!result.ok || !result.report.accepted) {
          inFlight = false;
          busy("printerSubmit", false, "Save and finish", "Saving…");
          applyReport(PRINTER_FIELDS, result.report, "printerSummary");
          return;
        }
        startPolling();
      })
      .catch(function () {
        inFlight = false;
        busy("printerSubmit", false, "Save and finish", "Saving…");
        setText("printerSummary", "The display did not answer. Try again.");
        show(byId("printerSummary"), true);
      });
  }

  function load() {
    readStatus()
      .then(function (status) {
        if (render(status)) {
          startPolling();
        }
      })
      .catch(function () {
        setText("version", "—");
      });

    var wifiForm = byId("wifiForm");
    if (wifiForm) {
      wifiForm.addEventListener("submit", submitWifi);
    }

    var printerForm = byId("printerForm");
    if (printerForm) {
      printerForm.addEventListener("submit", submitPrinter);
    }

    var updateInstall = byId("updateInstall");
    if (updateInstall) {
      updateInstall.addEventListener("click", installUpdate);
    }

    // Show/hide for the two fields a person types blind.
    //
    // ⛔ PRESENTATION ONLY. It flips the input's type and the ARIA state and nothing else: the
    // value is never read, copied, re-submitted, logged or sent anywhere, and no application
    // state is touched. The browser keeps the value and the caret across a type change, and
    // focus returns to the field so the user can keep typing.
    //
    // ONE IMPLEMENTATION FOR BOTH. The Wi-Fi password's control is hardware-accepted; the API
    // key reuses it rather than growing a second one that could drift. Which glyph shows is
    // derived in CSS from aria-pressed, so this sets one attribute and the icon follows.
    function wireReveal(buttonId, inputId, noun) {
      var reveal = byId(buttonId);
      var input = byId(inputId);
      if (!reveal || !input) {
        return;
      }

      reveal.addEventListener("click", function () {
        var willShow = input.type === "password";
        input.type = willShow ? "text" : "password";

        var label = (willShow ? "Hide " : "Show ") + noun;
        reveal.setAttribute("aria-pressed", willShow ? "true" : "false");
        reveal.setAttribute("aria-label", label);
        reveal.setAttribute("title", label);

        input.focus();
      });
    }

    wireReveal("wifiPasswordReveal", "wifiPassword", "password");
    wireReveal("printerApiKeyReveal", "printerApiKey", "API key");

    // A field stops being marked as soon as the user edits it.
    WIFI_FIELDS.concat(PRINTER_FIELDS).forEach(function (field) {
      var input = byId(field);
      if (input) {
        input.addEventListener("input", function () {
          clearField(field);
        });
      }
    });
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", load);
  } else {
    load();
  }
})();
