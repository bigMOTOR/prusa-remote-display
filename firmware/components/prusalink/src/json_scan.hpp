// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "json_reader/reader.hpp"

// Preserve the existing printer decoder interface and permissive accessors.
namespace prd::prusalink {
using json_reader::find;
using json_reader::find_object;
using json_reader::JsonField;
using json_reader::JsonObject;
using json_reader::JsonType;
using json_reader::kMaxDepth;
using json_reader::log_label;
using json_reader::parse_object;
using json_reader::read_number;
using json_reader::read_string;
using json_reader::ScanStatus;
} // namespace prd::prusalink
