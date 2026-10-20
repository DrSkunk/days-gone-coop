#pragma once
#include "session.hpp"
#include "ue.hpp"

#include <string>
#include <vector>

namespace coop::ui {

struct Command {
    session::Action action;
    size_t index = 0;
};

// Native Days Gone menus: a CO-OP row on the title menu, a CO-OP category in the pause menu, the session
// pages rendered into the game's own widgets, and HUD notifications for session events.
void Configure(bool trace);
// Before the original ProcessEvent; true swallows the event (the game never sees it).
bool BeforeEvent(ue::UObject* obj, ue::UObject* fn, void* parms);
// After the original ProcessEvent.
void AfterEvent(ue::UObject* obj, ue::UObject* fn);
// Once per mod tick on the game thread.
void Update(double now, const session::View& view, const session::EventLog& log);
std::vector<Command> TakeCommands();
// The page the open CO-OP menu shows (Root when closed).
session::Page CurrentPage();
bool MenuOpen();
// Shows a native HUD notification; false when no HUD is available (title menu, loading).
bool Notify(const std::string& title, const std::string& body);

} // namespace coop::ui
