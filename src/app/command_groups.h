// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app/state.h"
#include "core/command.h"

namespace noise::app {

// Each group registers its own commands and owns the argument parsing they need;
// `register_commands` is the one place the whole surface is assembled.
void register_document_commands(AppState& state, CommandTable& table);
void register_ai_commands(AppState& state, CommandTable& table);
void register_ai_job_commands(AppState& state, CommandTable& table);
void register_ai_intent_commands(AppState& state, CommandTable& table);
void register_ai_verb_commands(AppState& state, CommandTable& table);
void register_paint_commands(AppState& state, CommandTable& table);
void register_layer_commands(AppState& state, CommandTable& table);
void register_select_commands(AppState& state, CommandTable& table);
void register_mask_commands(AppState& state, CommandTable& table);
void register_view_commands(AppState& state, CommandTable& table);
void register_file_commands(AppState& state, CommandTable& table);

}  // namespace noise::app
