#include "native_ui.hpp"
#include "log.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>

namespace coop::ui {

using namespace ue;
using session::Action;
using session::Page;

namespace {

// Raw engine containers. FText is a shared reference to its text data plus flags.
struct FText24 {
    uint8_t bytes[24];
    void* Data() const {
        void* p;
        std::memcpy(&p, bytes, sizeof p);
        return p;
    }
};
struct FStringRaw {
    const wchar_t* data;
    int32_t num; // including the terminator
    int32_t max;
};
struct TArrayRaw {
    uint8_t* data;
    int32_t num;
    int32_t max;
};
struct CanvasLayout {
    float left, top, right, bottom;
    float minX, minY, maxX, maxY;
    float alignX, alignY;
};
static_assert(sizeof(FText24) == 24 && sizeof(FStringRaw) == 16 && sizeof(TArrayRaw) == 16);

// ESlateVisibility in UE 4.10.
constexpr uint8_t kVisible = 0, kCollapsed = 1, kHidden = 2, kSelfHitTestInvisible = 4;

bool g_trace = false;
std::vector<Command> g_commands;

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

std::string Narrow(const wchar_t* text, int length) {
    if (!text || length <= 0) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), n, nullptr, nullptr);
    return out;
}

UObject* TextLibrary() {
    static UObject* library = nullptr;
    if (!IsLive(library)) library = FindDefaultObject("KismetTextLibrary");
    return library;
}

// A new FText owning one reference. Moving it into an engine container hands that reference over;
// passing it as a call input lets the engine copy it, so the original reference is simply kept (leaked).
bool NewText(const std::string& text, FText24& out) {
    auto library = TextLibrary();
    if (!library) return false;
    std::wstring wide = Widen(text);
    FStringRaw input{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)};
    Call call(library, "Conv_StringToText");
    if (!call.set("InString", input).invoke()) return false;
    out = call.get<FText24>("ReturnValue");
    return out.Data() != nullptr;
}

// Diagnostics only: the returned engine string buffer is leaked.
std::string TextString(const FText24& text) {
    auto library = TextLibrary();
    if (!library || !text.Data()) return {};
    Call call(library, "Conv_TextToString");
    if (!call.set("InText", text).invoke()) return {};
    auto value = call.get<FStringRaw>("ReturnValue");
    return value.data && value.num > 1 && value.num < 4096 ? Narrow(value.data, value.num - 1) : std::string();
}

bool SetText(UObject* block, const std::string& text) {
    FText24 value;
    return IsLive(block) && NewText(text, value) && Call(block, "SetText").set("InText", value).invoke();
}

void SetVisibility(UObject* widget, uint8_t visibility) {
    if (IsLive(widget)) Call(widget, "SetVisibility").set("InVisibility", visibility).invoke();
}

template <typename T> T* Field(UObject* obj, const char* name) {
    if (!IsLive(obj)) return nullptr;
    auto property = FindChild(obj->cls, name);
    if (!property) return nullptr;
    int offset = PropertyOffset(property);
    return offset > 0 ? reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(obj) + offset) : nullptr;
}

UObject* ObjectField(UObject* obj, const char* name) {
    auto value = Field<UObject*>(obj, name);
    return value && IsLive(*value) ? *value : nullptr;
}

// Blueprint classes load with their menus; look them up lazily and cheaply.
struct ClassRef {
    const char* name;
    UObject* cls = nullptr;
    double next = 0;
    UObject* Get(double now) {
        if (IsLive(cls)) return cls;
        if (now < next) return nullptr;
        next = now + 2;
        cls = FindClass(name);
        return cls;
    }
};
ClassRef g_mainClass{"UI_MainMenuWidget_C"}, g_pauseClass{"OptionsTopMenuWidget_C"}, g_hudClass{"UI_HudWidget_C"};
UObject* g_mainSeen = nullptr;
UObject* g_pauseSeen = nullptr;
UObject* g_hudSeen = nullptr;
double g_now = 0;

std::string RowsText(const std::vector<session::Row>& rows) {
    std::string text;
    for (const auto& row : rows) text += (text.empty() ? "" : " | ") + row.label;
    return text;
}

// Handles page navigation shared by both menus; returns true when the menu should close.
bool Navigate(Page& page, const session::Row& row) {
    switch (row.action) {
    case Action::Back:
        if (page == Page::Root) return true;
        page = Page::Root;
        return false;
    case Action::BrowseFriends: page = Page::Friends; break;
    case Action::BrowsePublic: page = Page::Public; break;
    case Action::BrowseLan: page = Page::Lan; break;
    case Action::HostSteam:
    case Action::HostLan:
    case Action::JoinFriend:
    case Action::JoinPublic:
    case Action::JoinLan:
    case Action::Leave:
        page = Page::Root;
        break;
    default: break;
    }
    g_commands.push_back({row.action, row.index});
    return false;
}

// ---- Title menu ---------------------------------------------------------------------------------------
// UI_MainMenuWidget_C draws its main page from MenuOptionNames_Text into TextEntry0-9 (rows 0-8 have mouse
// targets). The CO-OP row is appended to that array in place; the co-op pages temporarily replace its
// contents, so the game's own layout, highlight, navigation and sounds render them.
struct MainMenu {
    UObject* widget = nullptr;
    int coopRow = -1;
    void* coopText = nullptr;
    bool open = false;
    Page page = Page::Root;
    std::vector<FText24> saved;
    int savedMax = 0;
    std::vector<session::Row> rows;
    std::vector<std::string> labels;
    void* renderedData = nullptr;
    void* renderedFirst = nullptr;
    std::string status;
    std::shared_ptr<UObject> statusBlock;
    std::shared_ptr<UObject> statusBackground;
    bool loggedLabels = false;
} g_main;

TArrayRaw* MainNames() {
    return Field<TArrayRaw>(g_main.widget, "MenuOptionNames_Text");
}

int MainCursor() {
    auto cursor = Field<int32_t>(g_main.widget, "CursorPosition");
    return cursor ? *cursor : -1;
}

// The main page shows TextEntry0-9; the new game, game mode and arcade pages use their own text blocks.
bool MainPageActive() {
    auto active = Field<TArrayRaw>(g_main.widget, "ActiveTextFields");
    auto first = ObjectField(g_main.widget, "TextEntry0");
    if (!active || !first || active->num <= 0 || !active->data) return false;
    UObject* shown;
    std::memcpy(&shown, active->data, sizeof shown);
    return shown == first;
}

void MainRefresh() {
    Call(g_main.widget, "UpdateSelectedPageOptions").invoke();
    Call(g_main.widget, "MainLabelUpdate").invoke();
}

void MainSetCursor(int position) {
    Call(g_main.widget, "SetCursorPosition").set("Position", position).invoke();
}

UObject* MainStatusBlock() {
    return IsLive(g_main.statusBlock.get()) ? g_main.statusBlock.get() : nullptr;
}

void MainReset() {
    g_main = MainMenu{};
}

void MainInject() {
    auto names = MainNames();
    auto max = Field<int32_t>(g_main.widget, "MaxMenuOptions");
    if (!names || !max || !names->data || names->num <= 0) return;
    if (g_main.coopText && g_main.coopRow >= 0 && g_main.coopRow < names->num) {
        FText24 row;
        std::memcpy(&row, names->data + 24 * g_main.coopRow, sizeof row);
        if (row.Data() == g_main.coopText) return;
    }
    // Never reallocate the engine's array; the main page has 6 options and room for 22.
    if (names->num >= names->max || names->num + 1 > static_cast<int>(session::kMaxRows)) return;
    if (!g_main.loggedLabels) {
        g_main.loggedLabels = true;
        std::string labels;
        for (int i = 0; i < names->num; ++i) {
            FText24 label;
            std::memcpy(&label, names->data + 24 * i, sizeof label);
            labels += (i ? " | " : "") + TextString(label);
        }
        Log("native ui: title menu rows: %s (max %d)", labels.c_str(), *max);
    }
    FText24 coop;
    if (!NewText("CO-OP", coop)) return;
    // Below the last game option and above QUIT GAME.
    int at = names->num - 1;
    std::memmove(names->data + 24 * (at + 1), names->data + 24 * at, 24);
    std::memcpy(names->data + 24 * at, &coop, sizeof coop);
    ++names->num;
    *max = names->num;
    g_main.coopRow = at;
    g_main.coopText = coop.Data();
    MainRefresh();
    Log("native ui: added CO-OP to the title menu at row %d of %d", at, names->num);
}

void ShowLogPanel(bool show, UObject* panel, UObject* tree, UObject* source, CanvasLayout area,
                  std::shared_ptr<UObject>& text, std::shared_ptr<UObject>& backdrop) {
    auto block = IsLive(text.get()) ? text.get() : nullptr;
    if (show && !block) {
        auto textClass = FindClass("TextBlock");
        Call spawn(FindDefaultObject("GameplayStatics"), "SpawnObject");
        if (!panel || !tree || !textClass ||
            !spawn.set("ObjectClass", textClass).set("Outer", tree).invoke()) {
            Log("native ui: cannot construct the session log");
            return;
        }
        block = spawn.get<UObject*>("ReturnValue");
        if (!IsLive(block)) return;
        text = RetainObject(block);
        if (auto wrap = Field<bool>(block, "AutoWrapText")) *wrap = true;
        Call background(FindDefaultObject("GameplayStatics"), "SpawnObject");
        if (background.set("ObjectClass", FindClass("Image")).set("Outer", tree).invoke()) {
            auto image = background.get<UObject*>("ReturnValue");
            if (IsLive(image)) {
                backdrop = RetainObject(image);
                struct Tint { float r, g, b, a; };
                Call(image, "SetColorAndOpacity").set("InColorAndOpacity", Tint{0, 0, 0, 0.8f}).invoke();
                Call addBackground(panel, "AddChildToCanvas");
                if (addBackground.set("Content", image).invoke()) {
                    auto backgroundSlot = addBackground.get<UObject*>("ReturnValue");
                    Call(backgroundSlot, "SetLayout").set("InLayoutData", area).invoke();
                    Call(backgroundSlot, "SetZOrder").set("InZOrder", 19).invoke();
                } else {
                    Log("native ui: cannot attach the lobby log background");
                }
            }
        } else {
            Log("native ui: cannot construct the lobby log background");
        }
        Call add(panel, "AddChildToCanvas");
        if (!add.set("Content", block).invoke()) {
            Log("native ui: cannot attach the session log");
            text.reset();
            SetVisibility(backdrop.get(), kCollapsed);
            return;
        }
        auto slot = add.get<UObject*>("ReturnValue");
        area.left += 16;
        area.top += 12;
        area.right += 16;
        area.bottom += 12;
        Call(slot, "SetLayout").set("InLayoutData", area).invoke();
        Call(slot, "SetZOrder").set("InZOrder", 20).invoke();
        struct Font { uint8_t bytes[64]; };
        struct Color { float r, g, b, a; uint8_t rest[24]; };
        if (auto font = Field<Font>(source, "Font")) {
            Font style = *font;
            auto size = FindChild(FindObject("SlateFontInfo"), "Size");
            int offset = size ? PropertyOffset(size) : -1;
            if (offset >= 0 && offset + static_cast<int>(sizeof(int32_t)) <= static_cast<int>(sizeof style)) {
                int32_t points = 28;
                std::memcpy(style.bytes + offset, &points, sizeof points);
            }
            Call(block, "SetFont").set("InFontInfo", style).invoke();
        }
        if (auto color = Field<Color>(source, "ColorAndOpacity")) {
            Color white = *color;
            white.r = white.g = white.b = white.a = 1;
            Call(block, "SetColorAndOpacity").set("InColorAndOpacity", white).invoke();
        }
        Log("native ui: created native session log on %s", Name(panel).c_str());
    }
    SetVisibility(block, show ? kSelfHitTestInvisible : kCollapsed);
    SetVisibility(backdrop.get(), show ? kSelfHitTestInvisible : kCollapsed);
}

void MainShowStatus(bool show) {
    ShowLogPanel(show, ObjectField(g_main.widget, "Menu_Layout"), ObjectField(g_main.widget, "WidgetTree"),
                 ObjectField(g_main.widget, "TextEntry0"), {0, 0, 0, 0, 0.48f, 0.54f, 0.95f, 0.87f, 0, 0},
                 g_main.statusBlock, g_main.statusBackground);
    if (show) g_main.status.clear();
}

void MainOpen() {
    auto names = MainNames();
    auto max = Field<int32_t>(g_main.widget, "MaxMenuOptions");
    if (!names || !max) return;
    g_main.saved.assign(reinterpret_cast<FText24*>(names->data), reinterpret_cast<FText24*>(names->data) + names->num);
    g_main.savedMax = *max;
    g_main.open = true;
    g_main.page = Page::Root;
    g_main.labels.clear();
    MainShowStatus(true);
    Log("native ui: CO-OP menu opened");
}

void MainClose(bool restore) {
    if (!g_main.open) return;
    g_main.open = false;
    auto names = MainNames();
    auto max = Field<int32_t>(g_main.widget, "MaxMenuOptions");
    if (restore && names && max && static_cast<int>(g_main.saved.size()) <= names->max) {
        // The page texts written over the originals stay referenced by nothing and are leaked (a few bytes).
        std::memcpy(names->data, g_main.saved.data(), g_main.saved.size() * sizeof(FText24));
        names->num = static_cast<int32_t>(g_main.saved.size());
        *max = g_main.savedMax;
        MainRefresh();
        MainSetCursor(g_main.coopRow);
    }
    g_main.saved.clear();
    g_main.labels.clear();
    MainShowStatus(false);
    Log("native ui: CO-OP menu closed");
}

void MainRender(const session::View& view, const std::vector<std::string>& log) {
    auto names = MainNames();
    auto max = Field<int32_t>(g_main.widget, "MaxMenuOptions");
    if (!names || !max) return;
    // The game rebuilt its options while a co-op page was showing: treat its new list as the originals.
    if (!g_main.labels.empty() && (names->data != g_main.renderedData || !names->num ||
                                   reinterpret_cast<FText24*>(names->data)->Data() != g_main.renderedFirst)) {
        Log("native ui: the title menu rebuilt its rows while CO-OP was open");
        g_main.saved.assign(reinterpret_cast<FText24*>(names->data), reinterpret_cast<FText24*>(names->data) + names->num);
        g_main.coopText = nullptr;
        g_main.labels.clear();
    }
    g_main.rows = session::BuildRows(g_main.page, view);
    std::vector<std::string> labels;
    for (const auto& row : g_main.rows) labels.push_back(row.label);
    if (labels != g_main.labels && static_cast<int>(labels.size()) <= names->max) {
        bool pageChanged = g_main.labels.size() != labels.size() || g_main.labels.empty() || g_main.labels.back() != labels.back();
        for (size_t i = 0; i < labels.size(); ++i) {
            if (i < g_main.labels.size() && g_main.labels[i] == labels[i] && static_cast<int>(i) < names->num) continue;
            FText24 text;
            if (!NewText(labels[i], text)) return;
            std::memcpy(names->data + 24 * i, &text, sizeof text);
        }
        names->num = static_cast<int32_t>(labels.size());
        *max = names->num;
        g_main.labels = labels;
        g_main.renderedData = names->data;
        g_main.renderedFirst = reinterpret_cast<FText24*>(names->data)->Data();
        int cursor = MainCursor();
        MainRefresh();
        MainSetCursor(pageChanged || cursor < 0 || cursor >= names->num ? 0 : cursor);
        Log("native ui: CO-OP page %s", RowsText(g_main.rows).c_str());
    }
    std::string status = std::string(session::PageTitle(g_main.page, view)) + "\n" + session::StatusText(g_main.page, view, log);
    if (status != g_main.status) {
        g_main.status = status;
        SetText(MainStatusBlock(), status);
    }
}

void MainActivate(int row) {
    if (row < 0 || row >= static_cast<int>(g_main.rows.size())) return;
    const auto& selected = g_main.rows[static_cast<size_t>(row)];
    Log("native ui: CO-OP selected '%s'", selected.label.c_str());
    if (selected.action == Action::Start) {
        // Continue the latest save through the game's own CONTINUE handling.
        MainClose(true);
        MainSetCursor(0);
        Call(g_main.widget, "OnConfirmPressed").invoke();
        g_commands.push_back({Action::Start});
        return;
    }
    if (Navigate(g_main.page, selected)) MainClose(true);
}

// Bound row events are named BndEvt__MenuInteract_MainMenu<N>_..._OnMenuInteract{Clicked,Hover}Event...
struct RowEvent {
    int row = -1;
    bool clicked = false;
};
RowEvent ParseRowEvent(const std::string& name) {
    constexpr std::string_view prefix = "BndEvt__MenuInteract_MainMenu";
    RowEvent event;
    if (!name.starts_with(prefix) || name.size() <= prefix.size() || !std::isdigit(static_cast<unsigned char>(name[prefix.size()])))
        return event;
    event.row = name[prefix.size()] - '0';
    event.clicked = name.find("ClickedEvent") != std::string::npos;
    return event;
}

enum class MainEvent : uint8_t { Other, Confirm, Cancel, Up, Down, Row };
struct MainEventInfo {
    MainEvent kind = MainEvent::Other;
    RowEvent row;
};

MainEventInfo ClassifyMain(UObject* fn) {
    static std::unordered_map<int32_t, MainEventInfo> cache;
    auto found = cache.find(fn->name.index);
    if (found != cache.end()) return found->second;
    std::string name = Name(fn);
    MainEventInfo info;
    if (name == "OnConfirmPressed") info.kind = MainEvent::Confirm;
    else if (name == "OnCancelPressed" || name == "NavigateBack") info.kind = MainEvent::Cancel;
    else if (name == "OnUpPressed") info.kind = MainEvent::Up;
    else if (name == "OnDownPressed") info.kind = MainEvent::Down;
    else if (auto row = ParseRowEvent(name); row.row >= 0) {
        info.kind = MainEvent::Row;
        info.row = row;
    }
    cache.emplace(fn->name.index, info);
    return info;
}

bool MainBefore(UObject* fn) {
    auto info = ClassifyMain(fn);
    if (info.kind == MainEvent::Other) return false;
    if (g_main.open) {
        switch (info.kind) {
        case MainEvent::Confirm: MainActivate(MainCursor()); return true;
        case MainEvent::Cancel: {
            session::Row back{"BACK", Action::Back};
            if (Navigate(g_main.page, back)) MainClose(true);
            return true;
        }
        case MainEvent::Row:
            if (info.row.clicked) {
                MainActivate(info.row.row);
                return true;
            }
            return info.row.row >= static_cast<int>(g_main.rows.size());
        default: return false;
        }
    }
    if (g_main.coopRow < 0 || !MainPageActive()) return false;
    if ((info.kind == MainEvent::Confirm && MainCursor() == g_main.coopRow) ||
        (info.kind == MainEvent::Row && info.row.clicked && info.row.row == g_main.coopRow)) {
        if (info.kind == MainEvent::Row) MainSetCursor(g_main.coopRow);
        MainOpen();
        return true;
    }
    return false;
}

void MainAfter(UObject* fn) {
    if (!g_main.open) return;
    auto info = ClassifyMain(fn);
    if (info.kind != MainEvent::Up && info.kind != MainEvent::Down) return;
    // The game wraps its cursor over its own option count; keep it on the co-op rows.
    int rows = static_cast<int>(g_main.rows.size()), cursor = MainCursor();
    if (rows > 0 && (cursor < 0 || cursor >= rows)) MainSetCursor(info.kind == MainEvent::Down ? 0 : rows - 1);
}

void MainUpdate(const session::View& view, const std::vector<std::string>& log) {
    UObject* widget = IsLive(g_mainSeen) ? g_mainSeen : nullptr;
    if (widget != g_main.widget) {
        MainReset();
        g_main.widget = widget;
    }
    if (!widget) return;
    if (g_main.open && !MainPageActive()) {
        Log("native ui: the title menu left its main page; closing CO-OP");
        g_main.open = false;
        g_main.saved.clear();
        g_main.labels.clear();
        MainShowStatus(false);
    }
    if (!g_main.open) {
        if (MainPageActive()) MainInject();
        return;
    }
    session::View title = view;
    title.inGame = false;
    MainRender(title, log);
}

// ---- Pause menu ---------------------------------------------------------------------------------------
// OptionsTopMenuWidget_C lists its options in a CategoryMenuWidget (entries filled by the native
// InitializeCategoryText). Each list index maps to ActiveMenuProfile[index]; the CO-OP entry is inserted
// above the quit options and later indices are shifted back before the game handles them. The co-op pages
// replace the list texts, the title and the selection status text while open.
struct PauseMenu {
    UObject* widget = nullptr;
    UObject* category = nullptr;
    int coopIndex = -1;
    void* coopText = nullptr;
    bool open = false;
    Page page = Page::Root;
    std::vector<std::string> savedLabels;
    std::string savedTitle;
    std::vector<session::Row> rows;
    std::vector<std::string> labels;
    std::string status;
    double handledAt = -10;   // a confirm or cancel handled by us; swallow its echoes in the same press
    double cancelAt = -10;
    int savedIndex = 0;
} g_pause;

struct PauseEvents {
    int32_t confirm = -1, cancel = -1, cancelTop = -1, highlight = -1, activate = -1, construct = -1;
} g_pauseNames;

TArrayRaw* PauseEntries() {
    return Field<TArrayRaw>(g_pause.category, "CategoryWidgets");
}

UObject* PauseEntry(int index) {
    auto entries = PauseEntries();
    if (!entries || index < 0 || index >= entries->num || !entries->data) return nullptr;
    UObject* entry;
    std::memcpy(&entry, entries->data + sizeof(UObject*) * index, sizeof entry);
    return IsLive(entry) ? entry : nullptr;
}

int PauseIndex() {
    auto index = Field<int32_t>(g_pause.category, "CategoryIndex");
    return index ? *index : -1;
}

bool InViewport(UObject* widget) {
    Call call(widget, "IsInViewport");
    return call.ok() && call.invoke() && call.get<bool>("ReturnValue");
}

bool PauseSetTexts(const std::vector<FText24>& texts) {
    TArrayRaw array{reinterpret_cast<uint8_t*>(const_cast<FText24*>(texts.data())), static_cast<int32_t>(texts.size()),
                    static_cast<int32_t>(texts.size())};
    return Call(g_pause.category, "InitializeCategoryText").set("Text", array).invoke();
}

void PauseSetIndex(int index) {
    Call(g_pause.category, "SetCategory").set("Index", index).invoke();
}

void PauseSetStatus(const std::string& text) {
    FText24 value;
    if (NewText(text, value)) Call(g_pause.widget, "SetSelectionStatusText").set("InText", value).invoke();
}

void PauseInject() {
    auto profile = Field<TArrayRaw>(g_pause.widget, "ActiveMenuProfile");
    auto entries = PauseEntries();
    if (!profile || !entries || profile->num < 2 || profile->num + 1 > entries->num) return;
    int count = profile->num;
    if (g_pause.coopText && g_pause.coopIndex >= 0) {
        auto entry = PauseEntry(g_pause.coopIndex);
        auto shown = entry ? Field<FText24>(entry, "DisplayText") : nullptr;
        if (shown && shown->Data() == g_pause.coopText) return;
    }
    // Fresh texts that own their references: the entries release their old texts while the list is
    // rewritten, and a text shifted down one entry may lose its last reference before it is copied.
    Log("native ui: pause inject start (profile %d, entries %d)", count, entries->num);
    std::vector<FText24> texts;
    for (int i = 0; i < count; ++i) {
        auto entry = PauseEntry(i);
        auto text = entry ? Field<FText24>(entry, "DisplayText") : nullptr;
        if (!text || !text->Data()) return;
        std::string label = TextString(*text);
        FText24 copy;
        Log("native ui: pause entry %d '%s'", i, label.c_str());
        if (label.empty() || !NewText(label, copy)) return;
        texts.push_back(copy);
    }
    FText24 coop;
    if (!NewText("CO-OP", coop)) return;
    // Above QUIT TO MAIN MENU / QUIT TO DESKTOP.
    int at = count - 2;
    texts.insert(texts.begin() + at, coop);
    int index = PauseIndex();
    Log("native ui: pause set texts (index %d)", index);
    if (!PauseSetTexts(texts)) return;
    Log("native ui: pause texts set");
    g_pause.coopIndex = at;
    g_pause.coopText = coop.Data();
    PauseSetIndex(index >= at ? index + 1 : std::max(index, 0));
    Log("native ui: added CO-OP to the pause menu at entry %d of %d", at, count + 1);
}

void PauseRender(const session::View& view, const std::vector<std::string>& log) {
    g_pause.rows = session::BuildRows(g_pause.page, view);
    std::vector<std::string> labels;
    for (const auto& row : g_pause.rows) labels.push_back(row.label);
    auto entries = PauseEntries();
    if (labels != g_pause.labels && entries && static_cast<int>(labels.size()) <= entries->num) {
        bool pageChanged = g_pause.labels.size() != labels.size() || g_pause.labels.empty() || g_pause.labels.back() != labels.back();
        std::vector<FText24> texts;
        for (const auto& label : labels) {
            FText24 text;
            if (!NewText(label, text)) return;
            texts.push_back(text);
        }
        int index = PauseIndex();
        if (!PauseSetTexts(texts)) return;
        g_pause.labels = labels;
        PauseSetIndex(pageChanged || index < 0 || index >= static_cast<int>(labels.size()) ? 0 : index);
        SetText(ObjectField(g_pause.widget, "OptionsTitle"), session::PageTitle(g_pause.page, view));
        Log("native ui: pause CO-OP page %s", RowsText(g_pause.rows).c_str());
    }
    std::string status = session::StatusText(g_pause.page, view, log);
    if (status != g_pause.status) {
        g_pause.status = status;
        PauseSetStatus(status);
    }
}

void PauseOpen() {
    g_pause.savedLabels.clear();
    auto entries = PauseEntries();
    auto profile = Field<TArrayRaw>(g_pause.widget, "ActiveMenuProfile");
    if (!entries || !profile) return;
    for (int i = 0; i <= profile->num && i < entries->num; ++i) {
        auto entry = PauseEntry(i);
        auto text = entry ? Field<FText24>(entry, "DisplayText") : nullptr;
        g_pause.savedLabels.push_back(text ? TextString(*text) : std::string());
    }
    auto title = ObjectField(g_pause.widget, "OptionsTitle");
    Call titleText(title, "GetText");
    g_pause.savedTitle = title && titleText.invoke() ? TextString(titleText.get<FText24>("ReturnValue")) : std::string();
    g_pause.savedIndex = PauseIndex();
    g_pause.open = true;
    g_pause.page = Page::Root;
    g_pause.labels.clear();
    g_pause.status.clear();
    Log("native ui: pause CO-OP opened");
}

void PauseClose() {
    if (!g_pause.open) return;
    g_pause.open = false;
    std::vector<FText24> texts;
    for (size_t i = 0; i < g_pause.savedLabels.size(); ++i) {
        FText24 text;
        if (!NewText(g_pause.savedLabels[i], text)) break;
        texts.push_back(text);
        if (static_cast<int>(i) == g_pause.coopIndex) g_pause.coopText = text.Data();
    }
    if (texts.size() == g_pause.savedLabels.size() && PauseSetTexts(texts)) PauseSetIndex(g_pause.coopIndex);
    if (!g_pause.savedTitle.empty()) SetText(ObjectField(g_pause.widget, "OptionsTitle"), g_pause.savedTitle);
    PauseSetStatus("");
    g_pause.labels.clear();
    Log("native ui: pause CO-OP closed");
}

void PauseActivate(int row) {
    if (row < 0 || row >= static_cast<int>(g_pause.rows.size())) return;
    const auto& selected = g_pause.rows[static_cast<size_t>(row)];
    Log("native ui: pause CO-OP selected '%s'", selected.label.c_str());
    if (Navigate(g_pause.page, selected)) PauseClose();
}

void PauseResolveNames() {
    if (g_pauseNames.confirm >= 0) return;
    g_pauseNames.confirm = FindNameIndex("OnConfirmPressed");
    g_pauseNames.cancel = FindNameIndex("OnCancelPressed");
    g_pauseNames.cancelTop = FindNameIndex("Cancel");
    g_pauseNames.highlight = FindNameIndex("BndEvt__OptionsCategoryWidget_K2Node_ComponentBoundEvent_85_OnSelectionModified__DelegateSignature");
    g_pauseNames.activate = FindNameIndex("BndEvt__OptionsCategoryWidget_K2Node_ComponentBoundEvent_398_OnSelectionModified__DelegateSignature");
    g_pauseNames.construct = FindNameIndex("Construct");
}

void PauseResetCategorySelection() {
    if (auto selected = Field<bool>(g_pause.category, "bCategorySelected")) *selected = false;
}

// Events on the pause menu (top) and its option list (category).
bool PauseBefore(UObject* obj, UObject* fn, void* parms) {
    PauseResolveNames();
    int32_t name = fn->name.index;
    bool top = obj == g_pause.widget;
    if (top && name == g_pauseNames.construct) {
        // The menu rebuilds its list each time it opens.
        g_pause.open = false;
        g_pause.labels.clear();
        g_pause.coopText = nullptr;
        return false;
    }
    if (g_pause.coopIndex < 0 || (!g_pause.coopText && !g_pause.open)) return false;
    bool echo = g_now - g_pause.handledAt < 0.25;
    if (!top && name == g_pauseNames.confirm) {
        int index = PauseIndex();
        if (g_pause.open) {
            g_pause.handledAt = g_now;
            PauseActivate(index);
            return true;
        }
        if (index == g_pause.coopIndex) {
            g_pause.handledAt = g_now;
            PauseOpen();
            return true;
        }
        return false;
    }
    if (top && name == g_pauseNames.confirm) return echo;
    if (name == g_pauseNames.cancel || (top && name == g_pauseNames.cancelTop)) {
        // The list and the top widget both see one press; the second must not close the pause menu.
        if (!g_pause.open) return g_now - g_pause.cancelAt < 0.25;
        if (g_now - g_pause.cancelAt >= 0.25) {
            g_pause.cancelAt = g_now;
            session::Row back{"BACK", Action::Back};
            if (Navigate(g_pause.page, back)) PauseClose();
        }
        return true;
    }
    if (top && (name == g_pauseNames.highlight || name == g_pauseNames.activate) && parms) {
        auto index = static_cast<int32_t*>(parms);
        bool activate = name == g_pauseNames.activate;
        bool selected = static_cast<uint8_t*>(parms)[4] != 0;
        if (g_pause.open) {
            // Mouse clicks activate without a confirm press.
            if (activate && selected && !echo) {
                g_pause.handledAt = g_now;
                PauseResetCategorySelection();
                PauseActivate(*index);
            } else if (activate) {
                PauseResetCategorySelection();
            }
            return true;
        }
        if (*index == g_pause.coopIndex) {
            if (activate) {
                PauseResetCategorySelection();
                if (selected && !echo) {
                    g_pause.handledAt = g_now;
                    PauseOpen();
                }
            }
            return true;
        }
        if (*index > g_pause.coopIndex) --*index;
    }
    return false;
}

void PauseUpdate(const session::View& view, const std::vector<std::string>& log) {
    UObject* widget = IsLive(g_pauseSeen) && !IsDefaultOrArchetype(g_pauseSeen) ? g_pauseSeen : nullptr;
    if (widget != g_pause.widget) {
        g_pause = PauseMenu{};
        g_pause.widget = widget;
    }
    if (!widget) return;
    g_pause.category = ObjectField(widget, "OptionsCategoryWidget");
    bool shown = view.inGame && g_pause.category && InViewport(widget);
    if (!shown) {
        if (g_pause.open) {
            g_pause.open = false;
            g_pause.labels.clear();
            g_pause.status.clear();
        }
        return;
    }
    if (!g_pause.open) {
        PauseInject();
        return;
    }
    PauseRender(view, log);
}

// ---- Tracing (MenuDiscovery) ----------------------------------------------------------------------

bool TraceClass(const std::string& name) {
    for (const char* token : {"OptionsTopMenu", "CategoryMenu", "HudWidget", "Notification", "PauseMenu", "UIManager"})
        if (name.find(token) != std::string::npos) return true;
    return false;
}

void Trace(UObject* obj, UObject* fn, void* parms) {
    static std::unordered_map<int32_t, bool> classes;
    auto found = classes.find(obj->cls->name.index);
    if (found == classes.end()) found = classes.emplace(obj->cls->name.index, TraceClass(Name(obj->cls))).first;
    if (!found->second) return;
    std::string fnName = Name(fn);
    if (fnName == "Tick" || fnName.starts_with("Get") || fnName.starts_with("Is")) return;
    static std::unordered_map<std::string, int> counts;
    std::string key = Name(obj->cls) + "." + fnName;
    if (++counts[key] > 12) return;
    std::string values;
    auto total = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(fn) + off::FunctionParmsSize);
    int listed = 0;
    for (auto p = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(fn) + off::StructChildren); p && listed < 8;
         p = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(p) + off::FieldNext), ++listed) {
        int offset = PropertyOffset(p);
        int size = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(p) + off::PropertyElementSize);
        std::string type = Name(p->cls);
        values += " " + Name(p) + ":" + type;
        if (parms && offset >= 0 && offset + 4 <= total && (type == "IntProperty" || type == "ByteProperty" || type == "BoolProperty" ||
                                                             type == "FloatProperty" || type == "ObjectProperty" || type == "StructProperty")) {
            auto at = static_cast<uint8_t*>(parms) + offset;
            if (type == "IntProperty") values += "=" + std::to_string(*reinterpret_cast<int32_t*>(at));
            else if (type == "FloatProperty") values += "=" + std::to_string(*reinterpret_cast<float*>(at));
            else if (type == "ObjectProperty") values += "=" + (IsLive(*reinterpret_cast<UObject**>(at)) ? Name(*reinterpret_cast<UObject**>(at)) : std::string("null"));
            else if (type == "StructProperty") {
                values += "[";
                for (int i = 0; i < std::min(size, 16); ++i) {
                    char hex[4];
                    std::snprintf(hex, sizeof hex, "%02x", at[i]);
                    values += hex;
                }
                values += "]";
            } else values += "=" + std::to_string(*at);
        }
    }
    Log("native ui trace: %s on %s%s", key.c_str(), Name(obj).c_str(), values.c_str());
}

// ---- HUD notifications --------------------------------------------------------------------------

UObject* g_hudLogOwner = nullptr;
std::shared_ptr<UObject> g_hudLogText, g_hudLogBackground;
double g_hudLogUntil = 0;
std::vector<std::string> g_hudLogLines;

UObject* FindHud() {
    if (IsLive(g_hudSeen) && !IsDefaultOrArchetype(g_hudSeen)) return g_hudSeen;
    static double next = 0;
    auto cls = g_hudClass.Get(g_now);
    if (!cls || g_now < next) return nullptr;
    next = g_now + 3;
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        auto obj = ObjectAt(i);
        if (obj && obj->cls == cls && IsLive(obj) && !IsDefaultOrArchetype(obj)) {
            g_hudSeen = obj;
            return obj;
        }
    }
    return nullptr;
}

} // namespace

void Configure(bool trace) {
    g_trace = trace;
}

bool BeforeEvent(UObject* obj, UObject* fn, void* parms) {
    if (!IsLive(obj) || !IsLive(fn) || !IsLive(obj->cls)) return false;
    if (g_trace) Trace(obj, fn, parms);
    if (obj->cls == g_mainClass.cls) {
        g_mainSeen = obj;
        if (obj == g_main.widget) return MainBefore(fn);
    } else if (obj->cls == g_pauseClass.cls) {
        g_pauseSeen = obj;
        if (obj == g_pause.widget) return PauseBefore(obj, fn, parms);
    } else if (g_pause.category && obj == g_pause.category) {
        return PauseBefore(obj, fn, parms);
    } else if (obj->cls == g_hudClass.cls) {
        g_hudSeen = obj;
    }
    return false;
}

void AfterEvent(UObject* obj, UObject* fn) {
    if (!IsLive(obj) || !IsLive(fn)) return;
    if (obj == g_main.widget) MainAfter(fn);
}

void Update(double now, const session::View& view, const session::EventLog& log) {
    g_now = now;
    g_mainClass.Get(now);
    g_pauseClass.Get(now);
    g_hudClass.Get(now);
    auto lines = log.Last(4);
    MainUpdate(view, lines);
    PauseUpdate(view, lines);
    bool showLog = view.inGame && !MenuOpen() && now < g_hudLogUntil;
    SetVisibility(g_hudLogText.get(), showLog ? kSelfHitTestInvisible : kCollapsed);
    SetVisibility(g_hudLogBackground.get(), showLog ? kSelfHitTestInvisible : kCollapsed);
}

std::vector<Command> TakeCommands() {
    std::vector<Command> out;
    out.swap(g_commands);
    return out;
}

Page CurrentPage() {
    return g_main.open ? g_main.page : g_pause.open ? g_pause.page : Page::Root;
}

bool MenuOpen() {
    return g_main.open || g_pause.open;
}

bool Notify(const std::string& title, const std::string& body) {
    auto hud = FindHud();
    if (!hud) {
        Log("native ui: cannot display session notification; HUD unavailable");
        return false;
    }
    if (hud != g_hudLogOwner) {
        SetVisibility(g_hudLogText.get(), kCollapsed);
        SetVisibility(g_hudLogBackground.get(), kCollapsed);
        g_hudLogText.reset();
        g_hudLogBackground.reset();
        g_hudLogOwner = hud;
        g_hudLogLines.clear();
    }
    ShowLogPanel(true, ObjectField(hud, "CanvasWrapper"), ObjectField(hud, "WidgetTree"),
                 ObjectField(g_main.widget, "TextEntry0"), {0, 0, 0, 0, 0.05f, 0.12f, 0.48f, 0.35f, 0, 0},
                 g_hudLogText, g_hudLogBackground);
    if (!IsLive(g_hudLogText.get())) return false;
    if (g_now >= g_hudLogUntil) g_hudLogLines.clear();
    g_hudLogLines.push_back(body);
    while (g_hudLogLines.size() > 4) g_hudLogLines.erase(g_hudLogLines.begin());
    std::string text = title;
    for (const auto& line : g_hudLogLines) text += "\n" + line;
    if (!SetText(g_hudLogText.get(), text)) {
        g_hudLogLines.pop_back();
        Log("native ui: cannot set session notification text");
        return false;
    }
    g_hudLogUntil = g_now + 6;
    return true;
}

} // namespace coop::ui
