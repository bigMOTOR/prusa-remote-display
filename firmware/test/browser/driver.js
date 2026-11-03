// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
//
// Runs INSIDE the real page, in a real browser, against the real app.js.
//
// Every check reads both halves of what the owner asked for: the actual DOM input value, and
// the state the user is shown — which means getComputedStyle, not a class name. A class is what
// the code intended; a computed colour is what the panel of glass actually did. The success
// summary that survived into an error had the right intent and the wrong result.

(function () {
  "use strict";

  var failures = [];
  var notes = {};

  // The page under test lives in the iframe. Same origin, so everything below reads the REAL
  // document, the REAL app.js state and the REAL computed styles — at a genuine 375 px width.
  var W = null;
  var D = null;

  function byId(id) {
    return D.getElementById(id);
  }
  function css(id, prop) {
    var e = byId(id);
    return e ? W.getComputedStyle(e)[prop] : "absent";
  }
  function check(label, actual, expected) {
    if (actual !== expected) {
      failures.push(label + ": expected " + JSON.stringify(expected) + ", got " + JSON.stringify(actual));
    }
  }
  function checkTruthy(label, value) {
    if (!value) failures.push(label + ": expected truthy, got " + JSON.stringify(value));
  }

  var ERROR = "rgb(224, 98, 98)";
  var SUCCESS = "rgb(92, 184, 92)";
  var ACCENT = "rgb(247, 127, 47)";

  var PRINTER = ["printerHost", "printerPort", "printerApiKey"];

  function marked(id) {
    var e = byId(id);
    return e.classList.contains("invalid");
  }
  function markedIds(ids) {
    return ids.filter(marked);
  }
  function submit(formId) {
    // Constructed in the frame's realm, or the page's listener never sees it as an Event.
    byId(formId).dispatchEvent(new W.Event("submit", { cancelable: true, bubbles: true }));
  }
  function type(id, value) {
    var e = byId(id);
    e.value = value;
    e.dispatchEvent(new W.Event("input", { bubbles: true }));
  }
  function wait(ms) {
    return new Promise(function (r) {
      setTimeout(r, ms);
    });
  }

  // --- Shared steps ---------------------------------------------------------

  function fillWifi(ssid, password) {
    byId("wifiSsid").value = ssid;
    byId("wifiPassword").value = password;
  }

  // Waits until the printer step is on screen, or gives up. The page reaches it by polling, so
  // this is the real asynchronous path rather than a shortcut.
  async function reachPrinterStep() {
    fillWifi("ProbeNet", "placeholder123");
    submit("wifiForm");
    for (var i = 0; i < 40; i += 1) {
      if (!byId("printerForm").hidden) return true;
      await wait(150);
    }
    return false;
  }


  // Waits for the page to become interactive again, i.e. NOT busy. Returns false if it never
  // does, which is the shape of the defect: an outcome the browser can never observe.
  async function waitInteractive(buttonId, ms) {
    var steps = Math.ceil(ms / 150);
    for (var i = 0; i < steps; i += 1) {
      if (!byId(buttonId).disabled) return true;
      await wait(150);
    }
    return false;
  }

  // --- Scenarios ------------------------------------------------------------

  async function printerFailure(expected) {
    checkTruthy("reached printer step", await reachPrinterStep());
    type("printerHost", "192.168.1.50");
    type("printerApiKey", "PLACEHOLDERKEY123");
    submit("printerForm");
    for (var i = 0; i < 40 && byId("wifiForm").hidden; i += 1) await wait(150);
    check("failure returned to Wi-Fi", byId("wifiForm").hidden, false);
    check("printer hidden until Wi-Fi recheck", byId("printerForm").hidden, true);
    check("specific recovery copy", byId("wifiSummary").textContent, expected);
    check("failure styled as error", css("wifiSummary", "borderLeftColor"), ERROR);
    check("retry enabled", byId("wifiSubmit").disabled, false);
    checkTruthy("mobile copy does not overflow", D.documentElement.scrollWidth <= W.innerWidth);
    if (new URLSearchParams(window.location.search).has("failure-shot")) return;
    checkTruthy("retained fields reappear after Wi-Fi recheck", await reachPrinterStep());
    check("host preserved", byId("printerHost").value, "192.168.1.50");
    check("port preserved", byId("printerPort").value, "80");
    check("key preserved", byId("printerApiKey").value, "PLACEHOLDERKEY123");
    check("key remains concealed", byId("printerApiKey").type, "password");
    check("printer retry enabled", byId("printerSubmit").disabled, false);
  }

  var scenarios = {
    "printer-unreachable": async function () {
      await printerFailure("That Wi-Fi worked, but the printer did not answer. Check its power, address and port. Check this Wi-Fi again, then correct the printer details.");
    },
    "printer-unauthorized": async function () {
      await printerFailure("That Wi-Fi worked, but the printer rejected the API key. Check the PrusaLink API key on the printer. Check this Wi-Fi again, then correct the key.");
    },
    "printer-unsupported": async function () {
      await printerFailure("That Wi-Fi worked, but the printer interface is not supported. Check that the address is for a supported PrusaLink printer. Check this Wi-Fi again before retrying.");
    },
    "printer-unusable-response": async function () {
      await printerFailure("That Wi-Fi worked, but the printer response could not be used. Check the PrusaLink address and printer firmware. Check this Wi-Fi again before retrying.");
    },
    "wifi-initial": async function () {
      check("ssid value", byId("wifiSsid").value, "");
      check("password value", byId("wifiPassword").value, "");
      check("password concealed", byId("wifiPassword").type, "password");
      check("printer step hidden", byId("printerForm").hidden, true);
      check("wifi summary hidden", byId("wifiSummary").hidden, true);
      check("done hidden", byId("done").hidden, true);
      check("submit label", byId("wifiSubmit").textContent, "Check this Wi-Fi");
      // The reveal is inside the field, not beside it.
      var ir = byId("wifiPassword").getBoundingClientRect();
      var br = byId("wifiPasswordReveal").getBoundingClientRect();
      checkTruthy("reveal inside field", br.right <= ir.right + 1 && br.left > ir.left);
      check("eye shown", css2(".reveal-eye", "display"), "block");
      check("eye-off hidden", css2(".reveal-eye-off", "display"), "none");
    },

    "wifi-empty": async function () {
      submit("wifiForm");
      await wait(400);
      check("ssid marked", marked("wifiSsid"), true);
      check("ssid aria", byId("wifiSsid").getAttribute("aria-invalid"), "true");
      check("ssid border is error", css("wifiSsid", "borderTopColor"), ERROR);
      check("ssid message", byId("wifiSsidError").textContent, "Enter your network name.");
      check("ssid message visible", byId("wifiSsidError").hidden, false);
      check("ssid message colour", css("wifiSsidError", "color"), ERROR);
      // An empty password is a legitimate open network and must NOT be marked.
      check("password not marked", marked("wifiPassword"), false);
      check("summary visible", byId("wifiSummary").hidden, false);
      check("summary text", byId("wifiSummary").textContent, "One field needs attention.");
      check("summary is error", css("wifiSummary", "borderLeftColor"), ERROR);
      check("still on step 1", byId("printerForm").hidden, true);
    },

    "wifi-invalid": async function () {
      fillWifi("ProbeNet", "short");
      submit("wifiForm");
      await wait(400);
      check("password marked", marked("wifiPassword"), true);
      check("password border", css("wifiPassword", "borderTopColor"), ERROR);
      check("password message", byId("wifiPasswordError").textContent,
        "A Wi-Fi password needs at least 8 characters.");
      check("ssid not marked", marked("wifiSsid"), false);
      check("DOM value preserved", byId("wifiPassword").value, "short");
      check("summary is error", css("wifiSummary", "borderLeftColor"), ERROR);
    },

    "wifi-validating": async function () {
      fillWifi("ProbeNet", "placeholder123");
      submit("wifiForm");
      await wait(1400);
      check("button busy label", byId("wifiSubmit").textContent, "Checking…");
      check("button disabled", byId("wifiSubmit").disabled, true);
      check("still on step 1", byId("printerForm").hidden, true);
      check("no fields marked", markedIds(["wifiSsid", "wifiPassword"]).length, 0);
    },

    "wifi-failure": async function () {
      fillWifi("ProbeNet", "placeholder123");
      submit("wifiForm");
      for (var i = 0; i < 40 && byId("wifiSummary").hidden; i += 1) await wait(150);
      check("summary visible", byId("wifiSummary").hidden, false);
      check("summary is error", css("wifiSummary", "borderLeftColor"), ERROR);
      check("summary text", byId("wifiSummary").textContent,
        "The display could not join that network. Check the name and the password.");
      check("button re-enabled", byId("wifiSubmit").disabled, false);
      check("button label restored", byId("wifiSubmit").textContent, "Check this Wi-Fi");
      check("step 1 still shown", byId("wifiForm").hidden, false);
      check("printer step hidden", byId("printerForm").hidden, true);
      check("values preserved", byId("wifiSsid").value, "ProbeNet");
    },

    "wifi-retry": async function () {
      fillWifi("ProbeNet", "wrongpassword");
      submit("wifiForm");
      for (var i = 0; i < 40 && byId("wifiSummary").hidden; i += 1) await wait(150);
      check("failed once", byId("wifiSummary").hidden, false);
      // Edit and resubmit — a recoverable failure, D-6.
      type("wifiPassword", "placeholder123");
      submit("wifiForm");
      var ok = false;
      for (var j = 0; j < 60; j += 1) {
        if (!byId("printerForm").hidden) { ok = true; break; }
        await wait(150);
      }
      check("retry reached printer step", ok, true);
      notes.submissions = 2;
    },

    "wifi-success": async function () {
      var ok = await reachPrinterStep();
      check("printer step shown", ok, true);
      check("wifi step hidden", byId("wifiForm").hidden, true);
      check("lead updated", byId("lead").textContent,
        "That Wi-Fi works. Now tell the display about your printer.");
      check("confirmation is success", css("printerSummary", "borderLeftColor"), SUCCESS);
      check("confirmation text", byId("printerSummary").textContent, "Connected to your Wi-Fi.");
    },

    // The owner's failing case: press Save the moment step 2 appears, touching nothing.
    "printer-initial": async function () {
      checkTruthy("reached printer step", await reachPrinterStep());
      // Port is a REAL value; Address is empty with the example in a hint.
      check("port DOM value", byId("printerPort").value, "80");
      check("address DOM value", byId("printerHost").value, "");
      check("address has no placeholder", byId("printerHost").getAttribute("placeholder"), null);
      check("port has no placeholder", byId("printerPort").getAttribute("placeholder"), null);
      check("api key empty", byId("printerApiKey").value, "");
      // ⬅️ THE EXAMPLE IS ON THE LABEL, at label size. Physical review rejected it as 12 px
      // tertiary text under the field: guidance for a required field cannot be the quietest type
      // on the page. Asserted as RENDERED size and colour, not as markup.
      var example = D.querySelector('label[for="printerHost"] .example');
      checkTruthy("address label carries the example", example !== null);
      checkTruthy("the example names a concrete address",
        example.textContent.indexOf("192.168.1.50") >= 0);
      check("example at label size", W.getComputedStyle(example).fontSize, "14px");
      check("example is tertiary, so it reads as guidance not data",
        W.getComputedStyle(example).color, "rgb(124, 132, 142)");

      // And the supporting detail below the field is readable rather than merely present.
      check("hint visible", byId("printerHostHint").hidden, false);
      check("hint is secondary", css("printerHostHint", "color"), "rgb(183, 189, 197)");
      check("hint at 13px", css("printerHostHint", "fontSize"), "13px");
      checkTruthy("hint says where to find it",
        byId("printerHostHint").textContent.indexOf("Settings") >= 0);

      // The API key must be actionable for someone who has never seen PrusaLink.
      var keyExample = D.querySelector('label[for="printerApiKey"] .example');
      checkTruthy("api key label names its source", keyExample !== null);
      checkTruthy("api key hint says where to get it",
        byId("printerHint").textContent.indexOf("PrusaLink") >= 0);
    },

    // --- Checkpoint 3d: the fixed unavailable answer must be VISIBLE -------------------------
    //
    // ⛔ THE DEFECT THIS EXISTS FOR SHOWED THE USER NOTHING. The 503 body parses, so the promise
    // resolves and .catch() never runs; the submit handler fell into the report branch, applyReport()
    // found no per-field outcomes, counted zero invalid fields and HID the summary. The button came
    // back and the page said nothing at all about a check that never started. A source-string
    // assertion cannot see that — only a browser can.
    "wifi-unavailable-503": async function () {
      fillWifi("ProbeNet", "placeholder123");
      submit("wifiForm");
      checkTruthy("became interactive again", await waitInteractive("wifiSubmit", 4000));

      check("summary visible", byId("wifiSummary").hidden, false);
      check("summary text", byId("wifiSummary").textContent,
        "The display could not start that check. Nothing was saved; try again.");
      check("summary is error", css("wifiSummary", "borderLeftColor"), ERROR);

      check("button re-enabled", byId("wifiSubmit").disabled, false);
      check("button label restored", byId("wifiSubmit").textContent, "Check this Wi-Fi");

      check("still on step 1", byId("wifiForm").hidden, false);
      check("printer step hidden", byId("printerForm").hidden, true);

      check("ssid preserved", byId("wifiSsid").value, "ProbeNet");
      check("password preserved", byId("wifiPassword").value, "placeholder123");
      check("no field marked invalid", markedIds(["wifiSsid", "wifiPassword"]).length, 0);
      check("ssid message empty", byId("wifiSsidError").textContent, "");
      check("password message empty", byId("wifiPasswordError").textContent, "");

      // ⛔ AND IT MUST NOT SAY THE DISPLAY WENT QUIET. It answered — with a 503.
      checkTruthy("does not claim silence",
        byId("wifiSummary").textContent.indexOf("did not answer") === -1);
      // Nor may it leak why: allocation and queue state are not actionable and not the user's.
      ["unavailable", "alloc", "queue", "heap"].forEach(function (word) {
        checkTruthy("no internal reason leaked: " + word,
          byId("wifiSummary").textContent.toLowerCase().indexOf(word) === -1);
      });

      // Retry must remain possible: the form still submits, which is the whole of the recovery.
      submit("wifiForm");
      await wait(400);
      check("retry was accepted by the page", byId("wifiSummary").hidden, false);
    },

    "printer-unavailable-503": async function () {
      checkTruthy("reached printer step", await reachPrinterStep());
      byId("printerHost").value = "192.0.2.10";
      byId("printerPort").value = "80";
      byId("printerApiKey").value = "placeholderkey";
      submit("printerForm");
      checkTruthy("became interactive again", await waitInteractive("printerSubmit", 4000));

      check("summary visible", byId("printerSummary").hidden, false);
      check("summary text", byId("printerSummary").textContent,
        "The display could not start that check. Nothing was saved; try again.");
      check("summary is error not success", css("printerSummary", "borderLeftColor"), ERROR);

      check("button re-enabled", byId("printerSubmit").disabled, false);
      check("button label restored", byId("printerSubmit").textContent, "Save and finish");

      check("still on step 2", byId("printerForm").hidden, false);
      check("step 1 stays hidden", byId("wifiForm").hidden, true);

      check("host preserved", byId("printerHost").value, "192.0.2.10");
      check("port preserved", byId("printerPort").value, "80");
      check("key preserved", byId("printerApiKey").value, "placeholderkey");
      check("no field marked invalid", markedIds(PRINTER).length, 0);

      checkTruthy("does not claim silence",
        byId("printerSummary").textContent.indexOf("did not answer") === -1);
    },

    "printer-empty": async function () {
      checkTruthy("reached printer step", await reachPrinterStep());
      byId("printerPort").value = "";
      submit("printerForm");
      await wait(500);
      check("all three marked", markedIds(PRINTER).length, 3);
      check("summary text", byId("printerSummary").textContent, "3 fields need attention.");
      check("summary is error not success", css("printerSummary", "borderLeftColor"), ERROR);
      check("host message", byId("printerHostError").textContent, "Enter your printer's address.");
      check("port message", byId("printerPortError").textContent, "Enter the port. It is usually 80.");
      check("key message", byId("printerApiKeyError").textContent, "Enter your printer's API key.");
    },

    "printer-missing-key": async function () {
      checkTruthy("reached printer step", await reachPrinterStep());
      byId("printerHost").value = "192.168.1.50";
      submit("printerForm");
      await wait(500);
      check("only the key marked", markedIds(PRINTER).join(","), "printerApiKey");
      check("host not marked", marked("printerHost"), false);
      check("port not marked", marked("printerPort"), false);
      check("host border stays neutral", css("printerHost", "borderTopColor"), "rgb(124, 132, 142)");
      check("key border is error", css("printerApiKey", "borderTopColor"), ERROR);
      check("summary text", byId("printerSummary").textContent, "One field needs attention.");
      check("summary is error", css("printerSummary", "borderLeftColor"), ERROR);
      check("host value kept", byId("printerHost").value, "192.168.1.50");
      check("port value kept", byId("printerPort").value, "80");
    },

    "printer-all-valid": async function () {
      checkTruthy("reached printer step", await reachPrinterStep());
      byId("printerHost").value = "192.168.1.50";
      byId("printerApiKey").value = "PLACEHOLDERKEY123";
      submit("printerForm");
      var ok = false;
      for (var i = 0; i < 60; i += 1) {
        if (!byId("done").hidden) { ok = true; break; }
        await wait(150);
      }
      check("reached done", ok, true);
      check("no field marked", markedIds(PRINTER).length, 0);
      check("printer form hidden", byId("printerForm").hidden, true);
      check("done is success", css("done", "borderLeftColor"), SUCCESS);
    },

    "summary-success-then-error": async function () {
      checkTruthy("reached printer step", await reachPrinterStep());
      check("starts as success", css("printerSummary", "borderLeftColor"), SUCCESS);
      notes.successClass = byId("printerSummary").className;
      byId("printerPort").value = "";
      submit("printerForm");
      await wait(500);
      // The whole point: the same element must not still be wearing success.
      check("becomes error", css("printerSummary", "borderLeftColor"), ERROR);
      checkTruthy("no success class remains",
        byId("printerSummary").className.indexOf("good") === -1);
      check("not the accent either", css("printerSummary", "borderLeftColor") === ACCENT, false);
    },

    "edit-clears-only-edited": async function () {
      checkTruthy("reached printer step", await reachPrinterStep());
      byId("printerPort").value = "";
      submit("printerForm");
      await wait(500);
      check("three marked first", markedIds(PRINTER).length, 3);
      type("printerHost", "192.168.1.50");
      await wait(80);
      check("edited field cleared", marked("printerHost"), false);
      check("its message gone", byId("printerHostError").hidden, true);
      check("others still marked", markedIds(PRINTER).join(","), "printerPort,printerApiKey");
      check("their messages remain", byId("printerApiKeyError").hidden, false);
    },

    "whitespace": async function () {
      checkTruthy("reached printer step", await reachPrinterStep());
      byId("printerHost").value = "  192.168.1.50\n";
      byId("printerPort").value = " 80 ";
      byId("printerApiKey").value = "PLACEHOLDERKEY123\n";
      submit("printerForm");
      var ok = false;
      for (var i = 0; i < 60; i += 1) {
        if (!byId("done").hidden) { ok = true; break; }
        await wait(150);
      }
      check("accepted despite whitespace", ok, true);
      check("no field marked", markedIds(PRINTER).length, 0);
      // Written back, so the field shows what was validated.
      check("host normalised in DOM", byId("printerHost").value, "192.168.1.50");
      check("port normalised in DOM", byId("printerPort").value, "80");
      check("key normalised in DOM", byId("printerApiKey").value, "PLACEHOLDERKEY123");
    },

    "double-submit": async function () {
      fillWifi("ProbeNet", "placeholder123");
      submit("wifiForm");
      await wait(120);
      check("disabled after first press", byId("wifiSubmit").disabled, true);
      submit("wifiForm");
      submit("wifiForm");
      await wait(900);
      notes.wifiPosts = "see server";
    },

    "polling-race": async function () {
      // "editing" is returned while the submission is in flight, which used to stop the timer
      // permanently and leave the page waiting for ever.
      fillWifi("ProbeNet", "placeholder123");
      submit("wifiForm");
      var ok = false;
      for (var i = 0; i < 80; i += 1) {
        if (!byId("printerForm").hidden) { ok = true; break; }
        await wait(150);
      }
      check("survived an editing poll mid-flight", ok, true);
    },

    // --- The failure / recovery matrix --------------------------------------

    "wifi-wrong-password-bounded": async function () {
      fillWifi("ProbeNet", "wrongpassword");
      submit("wifiForm");

      // THE INVARIANT: an accepted submission must reach a named terminal state and hand the
      // page back. Never an indefinite Checking….
      check("became interactive again", await waitInteractive("wifiSubmit", 12000), true);
      check("summary shown", byId("wifiSummary").hidden, false);
      check("summary is an error", css("wifiSummary", "borderLeftColor"), ERROR);
      check("named reason, not a generic one", byId("wifiSummary").textContent,
        "The display could not join that network. Check the name and the password.");
      check("button label restored", byId("wifiSubmit").textContent, "Check this Wi-Fi");
      check("still on step 1", byId("printerForm").hidden, true);
      check("value preserved for editing", byId("wifiPassword").value, "wrongpassword");
    },

    // --- The owner's DriverStalled ruling ------------------------------------

    "wifi-driver-stalled": async function () {
      fillWifi("ProbeNet", "placeholder123");
      submit("wifiForm");

      check("became interactive again", await waitInteractive("wifiSubmit", 12000), true);
      check("summary shown", byId("wifiSummary").hidden, false);
      check("summary is an error", css("wifiSummary", "borderLeftColor"), ERROR);

      // ⛔ THE EXACT WORDS THE OWNER RULED, AND THE ONLY ACTION THAT WORKS. Not "check the name
      // and the password": neither was tested, because the driver never answered. Not "try
      // again": the station is quarantined and a retry could not be judged either.
      check("names the condition and the action", byId("wifiSummary").textContent,
        "Wi-Fi unavailable. Restart the display.");

      check("does not blame the credential", byId("wifiSummary").textContent.indexOf("password"),
        -1);
      check("does not invite a retry that cannot be judged",
        byId("wifiSummary").textContent.indexOf("try again"), -1);

      // No internal reason is disclosed: not the driver call, not the ring, not the deadline.
      var leaked = ["driver", "stalled", "evidence", "ring", "deadline", "esp", "timeout"];
      for (var i = 0; i < leaked.length; i += 1) {
        checkTruthy("no internal reason leaked: " + leaked[i],
          byId("wifiSummary").textContent.toLowerCase().indexOf(leaked[i]) === -1);
      }

      check("button label restored", byId("wifiSubmit").textContent, "Check this Wi-Fi");
      check("still on step 1", byId("printerForm").hidden, true);
      check("value preserved", byId("wifiSsid").value, "ProbeNet");
    },

    "wifi-stall-is-not-a-credential-failure": async function () {
      // The two reasons the user can actually be told apart, asserted against each other rather
      // than each in isolation: an isolated assertion passes even if both strings became the same.
      fillWifi("ProbeNet", "placeholder123");
      submit("wifiForm");

      check("became interactive again", await waitInteractive("wifiSubmit", 12000), true);
      var stalled = byId("wifiSummary").textContent;

      check("the stall text is the actionable one", stalled,
        "Wi-Fi unavailable. Restart the display.");

      // ⛔ AND IT IS NOT THE CREDENTIAL FAILURE'S TEXT. That is the sentence the page shows for
      // wifi-unreachable, and showing it here would send the user to retype a password the display
      // never tested.
      checkTruthy("stall text differs from the credential-failure text",
        stalled !== "The display could not join that network. Check the name and the password.");

      // Nor the generic fallback, which would lose the instruction entirely.
      checkTruthy("stall text differs from the generic fallback",
        stalled !== "The display could not complete that request. Check the details you entered and try again.");

      check("still on step 1", byId("printerForm").hidden, true);
    },

    "wifi-evidence-lost": async function () {
      // A different internal cause with the same truthful outcome: the display cannot judge this
      // transaction and cannot judge another one either.
      fillWifi("ProbeNet", "placeholder123");
      submit("wifiForm");

      check("became interactive again", await waitInteractive("wifiSubmit", 12000), true);
      check("summary is an error", css("wifiSummary", "borderLeftColor"), ERROR);
      check("same actionable outcome", byId("wifiSummary").textContent,
        "Wi-Fi unavailable. Restart the display.");
      check("still on step 1", byId("printerForm").hidden, true);
    },

    // --- Checkpoint 3e: the previous network could not be rejoined -----------

    "wifi-restore-failed": async function () {
      fillWifi("ProbeNet", "placeholder123");
      submit("wifiForm");

      check("became interactive again", await waitInteractive("wifiSubmit", 12000), true);
      check("summary shown", byId("wifiSummary").hidden, false);
      check("summary is an error", css("wifiSummary", "borderLeftColor"), ERROR);

      // ⛔ THE CONDITION AND THE ONE ACTION LEFT. The attempt was abandoned, the display tried to
      // rejoin the network it had been using, and that network did not come back. The only thing the
      // user can do is enter Wi-Fi details, and the sentence says exactly that.
      check("names the condition and the action", byId("wifiSummary").textContent,
        "The display could not rejoin your previous network. Your saved configuration was not " +
          "replaced; enter your Wi-Fi details again.");

      // ⛔ THE NARROW GUARANTEE, AND ONLY THAT ONE. This route can be reached AFTER the printer stage
      // was submitted, by which point both candidate records had been written to and verified in the
      // inactive flash slots — so "nothing was saved" would be false. What is true on every route is
      // that no activation pointer moved, so the configuration the display was running was not
      // replaced.
      checkTruthy("states the narrow guarantee",
        byId("wifiSummary").textContent.indexOf("Your saved configuration was not replaced") !== -1);

      // And it must not make the broader claims that are not true here.
      check("does not claim nothing was saved",
        byId("wifiSummary").textContent.indexOf("Nothing was saved"), -1);
      check("does not claim nothing was changed",
        byId("wifiSummary").textContent.indexOf("Nothing was changed"), -1);

      // No internal reason is disclosed: not the generations, not the store, not the withdrawal.
      var leaked = ["generation", "storage", "withdraw", "pair", "flash", "esp"];
      for (var i = 0; i < leaked.length; i += 1) {
        checkTruthy("no internal reason leaked: " + leaked[i],
          byId("wifiSummary").textContent.toLowerCase().indexOf(leaked[i]) === -1);
      }

      check("button label restored", byId("wifiSubmit").textContent, "Check this Wi-Fi");
      check("still on step 1", byId("printerForm").hidden, true);
      check("value preserved for editing", byId("wifiSsid").value, "ProbeNet");
    },

    "wifi-restore-failure-is-not-a-credential-failure": async function () {
      // ⛔ THE NEGATIVE CONTROL, AS A SCENARIO. Asserted against the other sentences rather than in
      // isolation: an isolated assertion still passes if two of them became the same string, and
      // the whole value of a distinct reason is that it does not read like the others.
      fillWifi("ProbeNet", "placeholder123");
      submit("wifiForm");

      check("became interactive again", await waitInteractive("wifiSubmit", 12000), true);
      var restore = byId("wifiSummary").textContent;

      // Not the credential failure: the previous network's password was the display's own, stored
      // and previously proven, so sending the user to check a password would be false.
      checkTruthy("restore text differs from the credential-failure text",
        restore !== "The display could not join that network. Check the name and the password.");
      check("does not blame a password", restore.indexOf("password"), -1);

      // Not the driver stall: the driver answered perfectly well, and telling the user to restart
      // the display would hide the one action that works.
      checkTruthy("restore text differs from the stall text",
        restore !== "Wi-Fi unavailable. Restart the display.");
      check("does not ask for a restart", restore.indexOf("Restart"), -1);

      // Nor the generic fallback, which would invite a retry of something that was not the problem.
      checkTruthy("restore text differs from the generic fallback",
        restore !== "The display could not complete that request. Check the details you entered and try again.");
      check("does not invite a bare retry", restore.indexOf("try again"), -1);

      check("still on step 1", byId("printerForm").hidden, true);
    },

    // --- Checkpoint 3e: the rollback target could not be established ---------

    "wifi-rollback-unavailable": async function () {
      fillWifi("ProbeNet", "placeholder123");
      submit("wifiForm");

      check("became interactive again", await waitInteractive("wifiSubmit", 12000), true);
      check("summary shown", byId("wifiSummary").hidden, false);
      check("summary is an error", css("wifiSummary", "borderLeftColor"), ERROR);

      // ⛔ THE NARROW GUARANTEE AND A RESTART, BECAUSE THREE ROUTES SHARE THIS REASON AND THEY LEAVE
      // THE DEVICE IN DIFFERENT STATES. Only the PREFLIGHT route leaves the radio untouched and
      // nothing written; the staging-disagreement route had already written candidate records to the
      // inactive slots and already joined the candidate, and the post-disturbance load-failure route
      // had already joined the candidate and is taking it down. So the page may claim only what holds
      // on all three — that no activation pointer moved — and a restart is the one action that can
      // change the outcome, because retyping would be refused again for the same reason.
      check("names the condition and the action", byId("wifiSummary").textContent,
        "The display could not verify its saved network. Your saved configuration was not " +
          "replaced; restart the display and try again.");

      checkTruthy("states the narrow guarantee",
        byId("wifiSummary").textContent.indexOf("Your saved configuration was not replaced") !== -1);

      // ⛔ AND IT MUST NOT GENERALISE THE PREFLIGHT-ONLY ACCOUNT TO THE OTHER TWO ROUTES.
      check("does not claim nothing was changed",
        byId("wifiSummary").textContent.indexOf("Nothing was changed"), -1);
      check("does not claim nothing was saved",
        byId("wifiSummary").textContent.indexOf("Nothing was saved"), -1);
      check("does not claim the radio was untouched",
        byId("wifiSummary").textContent.toLowerCase().indexOf("untouched"), -1);
      check("does not invite a bare retry", byId("wifiSummary").textContent.indexOf("try again;"),
        -1);

      // No internal reason is disclosed: not the generations, not the pointers, not the store.
      var leaked = ["generation", "pointer", "rollback", "pair", "flash", "nvs", "esp"];
      for (var i = 0; i < leaked.length; i += 1) {
        checkTruthy("no internal reason leaked: " + leaked[i],
          byId("wifiSummary").textContent.toLowerCase().indexOf(leaked[i]) === -1);
      }

      check("button label restored", byId("wifiSubmit").textContent, "Check this Wi-Fi");
      check("still on step 1", byId("printerForm").hidden, true);
    },

    "wifi-rollback-is-not-a-restore-failure": async function () {
      // ⛔ THE NEGATIVE CONTROL FOR THE PAIR THE PM SEPARATED. "The device could not work out what to
      // try" and "a restore was tried and the network did not come back" have different causes and
      // different actions. Asserted against each other, because an isolated assertion still passes
      // if a future edit collapsed the two sentences into one.
      fillWifi("ProbeNet", "placeholder123");
      submit("wifiForm");

      check("became interactive again", await waitInteractive("wifiSubmit", 12000), true);
      var rollback = byId("wifiSummary").textContent;

      checkTruthy("rollback text differs from the restore-failure text",
        rollback !== "The display could not rejoin your previous network. Your saved " +
          "configuration was not replaced; enter your Wi-Fi details again.");

      // ⬅️ AND THE ONE CLAUSE THEY SHARE IS THE NARROW GUARANTEE, WHICH IS CORRECT FOR BOTH. Asserted
      // so that "they differ" cannot be satisfied by one of them dropping the guarantee altogether.
      checkTruthy("both make the same narrow guarantee",
        rollback.indexOf("Your saved configuration was not replaced") !== -1);

      // It must not claim an attempt was made against the previous network.
      check("does not claim a rejoin was attempted", rollback.indexOf("rejoin"), -1);

      // Nor read as a credential problem, nor as the driver stall, nor as the generic fallback.
      checkTruthy("differs from the credential-failure text",
        rollback !== "The display could not join that network. Check the name and the password.");
      check("does not blame a credential", rollback.indexOf("password"), -1);
      checkTruthy("differs from the stall text",
        rollback !== "Wi-Fi unavailable. Restart the display.");
      checkTruthy("differs from the generic fallback",
        rollback !== "The display could not complete that request. Check the details you entered and try again.");

      check("still on step 1", byId("printerForm").hidden, true);
    },

    "wifi-wrong-then-corrected": async function () {
      fillWifi("ProbeNet", "wrongpassword");
      submit("wifiForm");
      check("first attempt terminated", await waitInteractive("wifiSubmit", 12000), true);
      check("first attempt failed", byId("wifiSummary").hidden, false);

      type("wifiPassword", "correcthorse");
      submit("wifiForm");

      var reached = false;
      for (var i = 0; i < 80; i += 1) {
        if (!byId("printerForm").hidden) { reached = true; break; }
        await wait(150);
      }
      check("corrected password succeeded without reload", reached, true);
      check("no reboot was needed", byId("wifiForm").hidden, true);
    },

    "wifi-wrong-a-then-wrong-b": async function () {
      fillWifi("ProbeNet", "wrongpassA");
      submit("wifiForm");
      check("A terminated", await waitInteractive("wifiSubmit", 12000), true);
      var firstText = byId("wifiSummary").textContent;

      // A SECOND wrong password must get its own full attempt and its own named failure — not be
      // refused because the previous one gave up. That was a real hardware defect once.
      type("wifiPassword", "wrongpassB");
      submit("wifiForm");
      check("B terminated too", await waitInteractive("wifiSubmit", 12000), true);
      check("B reported its own failure", byId("wifiSummary").textContent, firstText);
      check("B is an error", css("wifiSummary", "borderLeftColor"), ERROR);
      check("still recoverable", byId("wifiSubmit").disabled, false);
    },

    "wifi-nonexistent-ssid-timeout": async function () {
      fillWifi("NoSuchNetworkAnywhere", "placeholder123");
      submit("wifiForm");
      check("bounded, not indefinite", await waitInteractive("wifiSubmit", 12000), true);
      check("timeout has its own wording", byId("wifiSummary").textContent,
        "That network did not finish connecting. Check the password, or move the display closer to the router.");
      check("is an error", css("wifiSummary", "borderLeftColor"), ERROR);
    },

    // ⛔ THE 2026-08-18 HANG, REPRODUCED. The device panicked mid-attempt, rebooted, and came
    // back reporting "editing" with a new session token. The page could not tell that from
    // "not started yet" and polled for ever with the button stuck on Checking….
    // --- The persistence over-claims, closed as one pass ---------------------
    //
    // ⛔ WHAT THESE PIN. A user-facing sentence is a claim about EVERY reason that reaches it. Three
    // branches used to end in "Nothing was saved" or "Nothing was changed" for reasons whose own
    // semantics do not support it: a storage failure may already have written a candidate, an
    // interrupted activation may already have moved a pointer, and the fallback knows nothing at all.

    "storage-commit-failed": async function () {
      fillWifi("ProbeNet", "placeholder123");
      submit("wifiForm");

      check("became interactive again", await waitInteractive("wifiSubmit", 12000), true);
      check("summary is an error", css("wifiSummary", "borderLeftColor"), ERROR);

      // The candidate WAS written and verified; only the pointer failed to move. So the page may
      // claim the narrow guarantee and nothing wider, and the retry action is unchanged.
      check("names the condition and the action", byId("wifiSummary").textContent,
        "The display could not save that configuration. Your saved configuration was not " +
          "replaced; try again.");
      checkTruthy("states the narrow guarantee",
        byId("wifiSummary").textContent.indexOf("Your saved configuration was not replaced") !== -1);
      check("does not claim nothing was changed",
        byId("wifiSummary").textContent.indexOf("Nothing was changed"), -1);
      check("does not claim nothing was saved",
        byId("wifiSummary").textContent.indexOf("Nothing was saved"), -1);

      check("still on step 1", byId("printerForm").hidden, true);
    },

    "promotion-failed": async function () {
      fillWifi("ProbeNet", "placeholder123");
      submit("wifiForm");

      check("became interactive again", await waitInteractive("wifiSubmit", 12000), true);
      check("summary is an error", css("wifiSummary", "borderLeftColor"), ERROR);

      // ⛔ THE ONE FAILURE THAT MAY HAVE MOVED A POINTER. It must make NO persistence claim at all —
      // not even the narrow one — and must send the user to a restart so the display can finish
      // what it started.
      check("names the condition and the action", byId("wifiSummary").textContent,
        "The display could not finish saving the configuration. Restart the display before " +
          "trying setup again.");
      check("does not claim nothing was saved",
        byId("wifiSummary").textContent.indexOf("Nothing was saved"), -1);
      check("does not claim nothing was changed",
        byId("wifiSummary").textContent.indexOf("Nothing was changed"), -1);
      check("does not claim the saved configuration survived",
        byId("wifiSummary").textContent.indexOf("was not replaced"), -1);
      checkTruthy("asks for a restart",
        byId("wifiSummary").textContent.indexOf("Restart the display") !== -1);

      check("still on step 1", byId("printerForm").hidden, true);
    },

    "unknown-reason-fallback": async function () {
      fillWifi("ProbeNet", "placeholder123");
      submit("wifiForm");

      check("became interactive again", await waitInteractive("wifiSubmit", 12000), true);
      check("summary is an error", css("wifiSummary", "borderLeftColor"), ERROR);

      // ⛔ A FALLBACK EXISTS TO BE SAFE FOR REASONS NOBODY ANTICIPATED, and a guarantee about flash is
      // the one thing it can never be safe about. Today it also carries the four printer failures,
      // every one of which is published after both candidate records were written and verified.
      check("says only what it can know", byId("wifiSummary").textContent,
        "The display could not complete that request. Check the details you entered and try again.");
      check("does not claim nothing was saved",
        byId("wifiSummary").textContent.indexOf("Nothing was saved"), -1);
      check("does not claim nothing was changed",
        byId("wifiSummary").textContent.indexOf("Nothing was changed"), -1);
      check("does not claim the saved configuration survived",
        byId("wifiSummary").textContent.indexOf("was not replaced"), -1);

      check("still on step 1", byId("printerForm").hidden, true);
    },

    "device-restart-mid-attempt": async function () {
      fillWifi("ProbeNet", "placeholder123");
      submit("wifiForm");

      check("the page recovers instead of hanging", await waitInteractive("wifiSubmit", 12000),
        true);
      check("summary shown", byId("wifiSummary").hidden, false);
      check("says what happened", byId("wifiSummary").textContent,
        "Setup was interrupted when the display restarted. Check what the display is showing " +
          "before trying again.");
      check("is an error", css("wifiSummary", "borderLeftColor"), ERROR);

      // ⛔ THE PAGE CANNOT KNOW WHERE THE RESTART LANDED. Before any write, between the two candidate
      // writes, or after a pointer moved — all three are the same observation from here, so no
      // persistence claim of any kind may be made.
      check("does not claim nothing was saved",
        byId("wifiSummary").textContent.indexOf("Nothing was saved"), -1);
      check("does not claim nothing was changed",
        byId("wifiSummary").textContent.indexOf("Nothing was changed"), -1);
      check("does not claim the saved configuration survived",
        byId("wifiSummary").textContent.indexOf("was not replaced"), -1);

      // ⬅️ AND THE FOUR ACTIONS STAY DISTINCT. Storage says try again, promotion says restart, this
      // one says look at the display, and the fallback says review the details. Asserted here rather
      // than in isolation, because a collapse would otherwise pass every individual expectation.
      var restarted = byId("wifiSummary").textContent;
      checkTruthy("differs from the storage-failure text",
        restarted !== "The display could not save that configuration. Your saved configuration " +
          "was not replaced; try again.");
      checkTruthy("differs from the interrupted-promotion text",
        restarted !== "The display could not finish saving the configuration. Restart the " +
          "display before trying setup again.");
      checkTruthy("differs from the generic fallback",
        restarted !== "The display could not complete that request. Check the details you " +
          "entered and try again.");

      check("form is editable again", byId("wifiForm").hidden, false);
      check("credentials still there to retry", byId("wifiSsid").value, "ProbeNet");
    },

    reveal: async function () {
      // Both controls, asserted identically — the API key reuses the mechanism already
      // hardware-accepted for the Wi-Fi password, so the same checks must hold for both.
      var cases = [
        { button: "wifiPasswordReveal", input: "wifiPassword", noun: "password" },
        { button: "printerApiKeyReveal", input: "printerApiKey", noun: "API key" },
      ];

      // ⬅️ EACH CONTROL IS MEASURED WHILE ITS OWN STEP IS VISIBLE. Reaching step 2 hides step 1,
      // and a hidden field has a zero-size bounding rect and cannot take focus — so asserting
      // the Wi-Fi control's geometry afterwards measured nothing at all.
      for (var i = 0; i < cases.length; i += 1) {
        var c = cases[i];

        if (c.input === "printerApiKey") {
          checkTruthy("reached printer step", await reachPrinterStep());
        }
        var input = byId(c.input);
        var button = byId(c.button);
        var tag = c.input + ": ";

        input.value = "PLACEHOLDERVALUE1";

        check(tag + "concealed by default", input.type, "password");
        check(tag + "label", button.getAttribute("aria-label"), "Show " + c.noun);
        check(tag + "pressed", button.getAttribute("aria-pressed"), "false");

        // The affordance sits INSIDE the field, same as the accepted one.
        var ir = input.getBoundingClientRect();
        var br = button.getBoundingClientRect();
        checkTruthy(tag + "inside the field",
          br.right <= ir.right + 1 && br.left > ir.left && br.top >= ir.top - 1 &&
          br.bottom <= ir.bottom + 1);
        checkTruthy(tag + "touch target at least 44px", Math.round(br.width) >= 44);

        // Exactly one glyph, driven by aria-pressed in CSS.
        var eye = button.querySelector(".reveal-eye");
        var eyeOff = button.querySelector(".reveal-eye-off");
        check(tag + "eye shown", W.getComputedStyle(eye).display, "block");
        check(tag + "eye-off hidden", W.getComputedStyle(eyeOff).display, "none");

        button.click();

        check(tag + "revealed", input.type, "text");
        check(tag + "value preserved", input.value, "PLACEHOLDERVALUE1");
        check(tag + "label flipped", button.getAttribute("aria-label"), "Hide " + c.noun);
        check(tag + "pressed flipped", button.getAttribute("aria-pressed"), "true");
        check(tag + "eye hidden", W.getComputedStyle(eye).display, "none");
        check(tag + "eye-off shown", W.getComputedStyle(eyeOff).display, "block");
        check(tag + "focus returned to field", D.activeElement === input, true);

        button.click();

        check(tag + "concealed again", input.type, "password");
        check(tag + "value still preserved", input.value, "PLACEHOLDERVALUE1");
        check(tag + "label back", button.getAttribute("aria-label"), "Show " + c.noun);
        check(tag + "eye back", W.getComputedStyle(eye).display, "block");
      }

      // ⛔ AND NOT ONE SUBMISSION. Toggling either control must never resubmit; the run's
      // expectPosts is 1 — the single Wi-Fi submission that reached step 2 — and four toggles
      // above must add nothing to it.
      notes.togglesPerformed = 4;
    },

    // --- Owner ruling A: the page over the LAN ------------------------------
    //
    // Reached over the local network instead of the display's own setup network, the device
    // refuses every configuration change. The page must therefore offer none — the rule that
    // kept a Save action off it while nothing could be saved.
    "lan-read-only": async function () {
      // The forms go away. Both of them: the printer step is hidden at rest anyway, so
      // asserting only the Wi-Fi one would pass even if the printer step were shown.
      check("wifi form hidden", byId("wifiForm").hidden, true);
      check("printer form hidden", byId("printerForm").hidden, true);

      // ⛔ HIDDEN MEANS NOT RENDERED, NOT MERELY FLAGGED. `hidden` doing nothing was a real
      // defect on an SVG in this same page, which is why every visibility claim here is read
      // back from getComputedStyle rather than from the attribute that was set.
      check("wifi form not displayed", css("wifiForm", "display"), "none");
      check("printer form not displayed", css("printerForm", "display"), "none");

      // And the explanation is actually on screen.
      check("notice shown", byId("readOnly").hidden, false);
      checkTruthy("notice displayed", css("readOnly", "display") !== "none");
      checkTruthy("notice has text", byId("readOnly").textContent.trim().length > 20);

      // Neutral, not a success and not an error: nothing succeeded and nothing went wrong.
      check("notice is neutral", css("readOnly", "borderLeftColor"), ACCENT);

      // ⛔ IT MUST NOT NAME A GESTURE THIS FIRMWARE DOES NOT HAVE. Re-entering setup from the
      // device is M3.10's, and telling a user to hold a button that does nothing yet is the
      // lie this page's own rule forbids. Asserted so a future edit cannot quietly add it.
      var notice = byId("readOnly").textContent.toLowerCase();
      checkTruthy("notice promises no button gesture", notice.indexOf("button") === -1);
      checkTruthy("notice promises no hold", notice.indexOf("hold") === -1);

      // No submit control of any kind is left on screen.
      //
      // ⬅️ MEASURED AS A BOX, NOT AS A COMPUTED display. A descendant of a `display: none`
      // ancestor keeps its OWN computed display — the button still reports "inline-block" while
      // rendering nothing — so asserting that property here passed judgement on the wrong
      // element and failed against a page that was correct. getClientRects() is empty only when
      // the element genuinely generates no box, which is the question being asked.
      check("wifi submit generates no box", byId("wifiSubmit").getClientRects().length, 0);
      check("printer submit generates no box", byId("printerSubmit").getClientRects().length, 0);
      check("wifi ssid field generates no box", byId("wifiSsid").getClientRects().length, 0);
      check("api key field generates no box", byId("printerApiKey").getClientRects().length, 0);

      // The firmware version still renders: this surface is a status view, not a blank page.
      check("version shown", byId("version").textContent, "0.1.0-dev");

      // Give the page longer than one poll interval to change its mind. It must not.
      await wait(1600);
      check("wifi form still hidden", css("wifiForm", "display"), "none");
      check("notice still shown", byId("readOnly").hidden, false);
    },

    "lan-read-only-ignores-the-flow": async function () {
      // The device reports "succeeded" — a provisioned device's honest state. On this surface
      // that must not draw the success panel for somebody who submitted nothing.
      check("done hidden", byId("done").hidden, true);
      check("done not displayed", css("done", "display"), "none");
      check("notice shown", byId("readOnly").hidden, false);
      check("wifi form not displayed", css("wifiForm", "display"), "none");

      await wait(1600);
      check("done still hidden", byId("done").hidden, true);
      check("notice still shown", byId("readOnly").hidden, false);
    },

    "update-lan-available": async function () {
      check("update card shown", byId("updateCard").hidden, false);
      checkTruthy("update card displayed", css("updateCard", "display") !== "none");
      check("update version", byId("updateVersion").textContent, "0.2.0");
      check("install action hidden", byId("updateInstall").hidden, true);
      check("install action generates no box", byId("updateInstall").getClientRects().length, 0);
      check(
        "authorization instruction",
        byId("updateStatus").textContent,
        "Hold the display button to open its authorized settings network, then install here.",
      );
    },

    "update-authorized-install": async function () {
      check("update card shown", byId("updateCard").hidden, false);
      check("update version", byId("updateVersion").textContent, "0.2.0");
      check("install action shown", byId("updateInstall").hidden, false);
      checkTruthy("install action displayed", byId("updateInstall").getClientRects().length > 0);

      byId("updateInstall").click();
      for (var i = 0; i < 30 && !byId("updateInstall").hidden; i += 1) await wait(100);

      check("install action retired", byId("updateInstall").hidden, true);
      check(
        "install status",
        byId("updateStatus").textContent,
        "Installing on the display. Keep it powered.",
      );
    },
  };

  function css2(selector, prop) {
    var e = D.querySelector(selector);
    return e ? W.getComputedStyle(e)[prop] : "absent";
  }

  // Every scenario is also a mobile-layout check.
  //
  // ⬅️ MEASURED, NOT ASSUMED. The first screenshot looked clipped at the right edge, which is
  // either the page overflowing at phone width or the harness laying out at some other width and
  // cropping. Those need opposite fixes, so the run reports the numbers and fails on real
  // overflow rather than leaving it to be read off an image.
  function checkViewport() {
    var doc = D.documentElement;
    notes.viewport = W.innerWidth + "x" + W.innerHeight;
    notes.scrollWidth = doc.scrollWidth;
    notes.clientWidth = doc.clientWidth;

    check("viewport width is the iPhone width", W.innerWidth, 375);
    if (doc.scrollWidth > doc.clientWidth + 1) {
      failures.push(
        "horizontal overflow: scrollWidth " + doc.scrollWidth + " > clientWidth " + doc.clientWidth,
      );
    }
  }

  function frameReady() {
    return new Promise(function (resolve) {
      var frame = document.getElementById("frame");
      function ready() {
        W = frame.contentWindow;
        D = frame.contentDocument;
        // app.js wires its listeners on load; give the frame's own load a moment to settle.
        setTimeout(resolve, 150);
      }
      if (frame.contentDocument && frame.contentDocument.readyState === "complete") {
        ready();
      } else {
        frame.addEventListener("load", ready);
      }
    });
  }

  async function main() {
    await frameReady();

    var name = window.__scenario;
    var fn = scenarios[name];
    if (!fn) {
      failures.push("unknown scenario: " + name);
    } else {
      try {
        await fn();
        checkViewport();
      } catch (error) {
        failures.push("threw: " + (error && error.message ? error.message : String(error)));
      }
    }

    await fetch("/__result", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ scenario: name, failures: failures, notes: notes }),
    });
  }

  // After app.js has wired its listeners.
  if (document.readyState === "complete") {
    main();
  } else {
    window.addEventListener("load", main);
  }
})();
