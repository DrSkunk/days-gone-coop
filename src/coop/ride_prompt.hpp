// Native "hold to interact" prompt on the partner's motorcycle, built from the game's own
// BendBasicInteract actor so it looks and behaves like every other interaction.
#pragma once
#include "ue.hpp"

#include <string>

namespace coop::ride {

// Keeps the prompt at `at` while `visible`; returns true once each time the player completes it.
bool Update(ue::UObject* context, bool visible, const ue::FVector& at, const std::string& label);
void Destroy();
// Called from the ProcessEvent hook on the game thread.
void OnEvent(ue::UObject* obj, ue::UObject* fn);

} // namespace coop::ride
