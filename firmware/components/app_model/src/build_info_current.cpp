// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "app_model/build_info.hpp"

#include "app_model/generated/build_info_generated.hpp"

namespace prd::app_model {

BuildInfo current_build_info() {
    BuildInfo info;

    info.version = SemanticVersion::parse(generated::kProjectVersion).value_or(SemanticVersion{});
    info.git_commit = generated::kGitCommit;
    info.git_dirty = generated::kGitDirty;
    info.build_profile = generated::kBuildProfile;
    info.idf_version = generated::kIdfVersion;
    info.board_target = generated::kBoardTarget;

    return info;
}

} // namespace prd::app_model
