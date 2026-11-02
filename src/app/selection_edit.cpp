// SPDX-License-Identifier: Apache-2.0
#include "app/selection_edit.h"

#include "app/state.h"

namespace noise::app {
namespace {

bool result_layer_exists(const AppState& state) {
    if (state.selection_edit.layer_id == 0) {
        return true;
    }
    for (const Layer& layer : state.doc.layers()) {
        if (layer.id == state.selection_edit.layer_id) {
            return true;
        }
    }
    return false;
}

bool landed_result(const AppState& state) {
    const SelectionEdit& edit = state.selection_edit;
    if (edit.job.empty() || edit.document_epoch != state.document_epoch ||
        state.ai.jobs == nullptr || state.doc.empty() || !result_layer_exists(state)) {
        return false;
    }
    const std::optional<ai::JobRecord> record = state.ai.jobs->find(edit.job);
    return record.has_value() && record->state == ai::JobState::succeeded &&
           record->landing.landed;
}

}  // namespace

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
    return SelectionEditPhase::actions;
}

const char* selection_edit_phase_name(SelectionEditPhase phase) {
    switch (phase) {
        case SelectionEditPhase::actions:
            return "actions";
        case SelectionEditPhase::generating:
            return "generating";
    }
    return "actions";
}

bool selection_edit_hides_bar(const AppState& state) {
    return landed_result(state) &&
           state.doc.selection_revision() == state.selection_edit.landed_selection_revision;
}

void selection_edit_queued(AppState& state, const std::string& job) {
    state.selection_edit = SelectionEdit{job, state.document_epoch, 0, 0};
    state.ask_verb = std::nullopt;
}

void selection_edit_landed(AppState& state, const std::string& job) {
    SelectionEdit& edit = state.selection_edit;
    if (edit.job != job || edit.document_epoch != state.document_epoch || state.doc.empty()) {
        return;
    }
    edit.layer_id = state.active_layer().id;
    edit.landed_selection_revision = state.doc.selection_revision();
    // Landing never selects the mask or changes the active tool. A mask remains a thumbnail click away.
    state.edit_plane = LayerPlane::pixels;
}

void selection_edit_prepare_outpaint(AppState& state) {
    state.selection_edit = {};
    state.ask_verb = std::nullopt;
    state.edit_plane = LayerPlane::pixels;
    state.tool = "cover";
}

void selection_edit_reconcile(AppState& state) {
    if (state.selection_edit.job.empty() ||
        selection_edit_phase(state) == SelectionEditPhase::generating) {
        return;
    }
    if (state.selection_edit.document_epoch != state.document_epoch ||
        !result_layer_exists(state)) {
        state.selection_edit = {};
    }
}

}  // namespace noise::app
