// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

namespace prd::network::station {
class EventRing;

namespace test {
enum class Snapshot { Refused, Write };

// Host-only scheduler seam. No fields are added to EventRing; target builds do not
// include this header or call this function. The test pauses a consumer after a
// real atomic load and publishes a producer burst before the consumer resumes.
void after_load(EventRing& ring, Snapshot snapshot);
} // namespace test
} // namespace prd::network::station
