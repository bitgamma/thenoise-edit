// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

namespace noise {
struct AppState;
}

namespace noise::app {

struct SelectionEdit {
    std::string job;
    std::uint64_t document_epoch = 0;
    std::uint64_t layer_id = 0;
    std::uint64_t landed_selection_revision = 0;
};

enum class SelectionEditPhase { actions, generating };

SelectionEditPhase selection_edit_phase(const AppState& state);
const char* selection_edit_phase_name(SelectionEditPhase phase);
bool selection_edit_hides_bar(const AppState& state);
void selection_edit_queued(AppState& state, const std::string& job);
void selection_edit_landed(AppState& state, const std::string& job);
void selection_edit_prepare_outpaint(AppState& state);
void selection_edit_reconcile(AppState& state);

}  // namespace noise::app
