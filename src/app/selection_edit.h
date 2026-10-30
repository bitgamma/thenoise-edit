// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

namespace noise {
struct AppState;
struct Layer;
}

namespace noise::app {

// One selection edit from queueing through its editable result. The queue owns job progress;
// this records only which job should control the on-canvas edit affordance.
struct SelectionEdit {
    std::string job;
    std::uint64_t document_epoch = 0;
    std::uint64_t layer_id = 0;
};

enum class SelectionEditPhase { actions, generating, editing };

const Layer* selection_edit_result_layer(const AppState& state);
SelectionEditPhase selection_edit_phase(const AppState& state);
const char* selection_edit_phase_name(SelectionEditPhase phase);
void selection_edit_queued(AppState& state, const std::string& job);
void selection_edit_landed(AppState& state, const std::string& job);
void selection_edit_new(AppState& state);
// An erased result layer or replaced document must not leave an obsolete edit active.
void selection_edit_reconcile(AppState& state);

}  // namespace noise::app
