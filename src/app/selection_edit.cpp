// SPDX-License-Identifier: Apache-2.0
#include "app/selection_edit.h"

#include "app/state.h"

namespace noise::app {

// An AI result is one specific layer, not whichever masked layer is selected next.
// Looking it up also catches an Undo that removed the result while another mask still exists.
const Layer* selection_edit_result_layer(const AppState& state) {
    for (const Layer& layer : state.doc.layers()) {
        if (layer.id == state.selection_edit.layer_id && layer.mask.has_value()) {
            return &layer;
        }
    }
    return nullptr;
}

SelectionEditPhase selection_edit_phase(const AppState& state) {
    const SelectionEdit& edit = state.selection_edit;
    if (edit.job.empty() || edit.document_epoch != state.document_epoch ||
        state.ai.jobs == nullptr || state.doc.empty()) {
        return SelectionEditPhase::actions;
    }
    const std::optional<ai::JobRecord> record = state.ai.jobs->find(edit.job);
    if (!record.has_value()) {
        return SelectionEditPhase::actions;
    }
    if (!ai::job_state_is_terminal(record->state) ||
        (record->state == ai::JobState::succeeded && !record->landing.landed &&
         !record->landing.land_refused)) {
        return SelectionEditPhase::generating;
    }
    if (record->state == ai::JobState::succeeded && record->landing.landed &&
        edit.layer_id != 0 && state.coverage_target == CoverageTarget::mask &&
        selection_edit_result_layer(state) != nullptr) {
        return SelectionEditPhase::editing;
    }
    return SelectionEditPhase::actions;
}

const char* selection_edit_phase_name(SelectionEditPhase phase) {
    switch (phase) {
        case SelectionEditPhase::actions:
            return "actions";
        case SelectionEditPhase::generating:
            return "generating";
        case SelectionEditPhase::editing:
            return "editing";
    }
    return "actions";
}

void selection_edit_queued(AppState& state, const std::string& job) {
    state.selection_edit = SelectionEdit{job, state.document_epoch, 0};
    state.ask_verb = std::nullopt;
}

void selection_edit_landed(AppState& state, const std::string& job) {
    SelectionEdit& edit = state.selection_edit;
    if (edit.job != job || edit.document_epoch != state.document_epoch || state.doc.empty()) {
        return;
    }
    // A new result can edit its own boundary without regenerating. The special first answer on a
    // completely blank canvas has no result mask, so it cannot truthfully enter mask-edit mode.
    if (state.active_layer().mask.has_value()) {
        edit.layer_id = state.active_layer().id;
        state.coverage_target = CoverageTarget::mask;
    }
}

void selection_edit_new(AppState& state) {
    state.selection_edit = {};
    state.ask_verb = std::nullopt;
    state.coverage_target = CoverageTarget::marked;
}

void selection_edit_reconcile(AppState& state) {
    if (state.selection_edit.job.empty() ||
        selection_edit_phase(state) == SelectionEditPhase::generating) {
        return;
    }
    if (state.selection_edit.document_epoch != state.document_epoch ||
        (state.selection_edit.layer_id != 0 && selection_edit_result_layer(state) == nullptr)) {
        selection_edit_new(state);
    }
}

}  // namespace noise::app
