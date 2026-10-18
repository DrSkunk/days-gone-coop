#include "ue.hpp"
#include "log.hpp"

#include <windows.h>

#include <unordered_map>
#include <unordered_set>

namespace ue {

Globals g;

namespace {

constexpr int kNamesPerChunk = 16384;
constexpr int kNameChunks = 128;
constexpr int kNameEntryString = 0x12; // Bend adds a 16-bit field before the string (stock 4.10: 0x10)

bool Readable(const void* p, size_t len) {
    static uintptr_t lo = 0, hi = 0;
    auto a = reinterpret_cast<uintptr_t>(p);
    if (a < 0x10000 || a > 0x7ffffffffff0) return false;
    if (a >= lo && a + len <= hi) return true;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                         PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        return false;
    lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    hi = lo + mbi.RegionSize;
    return a + len <= hi;
}

const uint8_t* EntryAt(const uint8_t* names, int32_t index) {
    if (!names || index < 0) return nullptr;
    auto chunks = reinterpret_cast<uint8_t* const* const*>(names);
    int c = index / kNamesPerChunk;
    if (c >= kNameChunks || !chunks[c]) return nullptr;
    return reinterpret_cast<const uint8_t*>(chunks[c][index % kNamesPerChunk]);
}

bool EntryIs(const uint8_t* names, int32_t index, const char* text) {
    auto chunks = reinterpret_cast<uint8_t* const* const*>(names);
    if (!Readable(chunks, sizeof(void*))) return false;
    auto chunk = chunks[index / kNamesPerChunk];
    if (!Readable(chunk, sizeof(void*) * (index % kNamesPerChunk + 1))) return false;
    auto entry = reinterpret_cast<const uint8_t*>(chunk[index % kNamesPerChunk]);
    size_t n = std::strlen(text) + 1;
    if (!Readable(entry, kNameEntryString + n)) return false;
    return (*reinterpret_cast<const int32_t*>(entry) & 1) == 0 &&
           std::memcmp(entry + kNameEntryString, text, n) == 0;
}

int32_t NameCount() { return *reinterpret_cast<const int32_t*>(g.names + kNameChunks * 8); }

std::unordered_map<std::string, int32_t>& NameCache() {
    static std::unordered_map<std::string, int32_t> cache;
    return cache;
}

} // namespace

bool InitNames(std::string& diag) {
    auto base = reinterpret_cast<uint8_t*>(g.imageBase);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (std::memcmp(sec->Name, ".data", 6) != 0) continue;
        auto start = base + sec->VirtualAddress;
        auto end = start + sec->Misc.VirtualSize;
        for (auto p = start; p + 8 <= end; p += 8) {
            uint8_t* candidates[2] = {p, *reinterpret_cast<uint8_t**>(p)};
            for (uint8_t* c : candidates) {
                if (!Readable(c, kNameChunks * 8 + 8)) continue;
                if (!EntryIs(c, 0, "None") || !EntryIs(c, 1, "ByteProperty")) continue;
                int32_t count = *reinterpret_cast<int32_t*>(c + kNameChunks * 8);
                if (count < 1000 || count > kNamesPerChunk * kNameChunks) continue;
                g.names = c;
                char msg[160];
                std::snprintf(msg, sizeof msg, "GNames %s at rva 0x%llx, %d names",
                              c == p ? "inline" : "via pointer",
                              static_cast<unsigned long long>(p - base), count);
                diag = msg;
                return true;
            }
        }
    }
    diag = "GNames not found in .data";
    return false;
}

std::string NameString(int32_t index) {
    auto e = EntryAt(g.names, index);
    if (!e || !Readable(e, kNameEntryString + 1)) return "<bad-name>";
    if (*reinterpret_cast<const int32_t*>(e) & 1) {
        std::string out;
        for (auto w = reinterpret_cast<const wchar_t*>(e + kNameEntryString); *w; ++w)
            out.push_back(*w < 128 ? static_cast<char>(*w) : '?');
        return out;
    }
    return reinterpret_cast<const char*>(e + kNameEntryString);
}

int32_t FindNameIndex(const char* s) {
    auto& cache = NameCache();
    if (auto it = cache.find(s); it != cache.end()) return it->second;
    int32_t n = NameCount();
    for (int32_t i = 0; i < n; ++i) {
        auto e = EntryAt(g.names, i);
        if (e && !(*reinterpret_cast<const int32_t*>(e) & 1) &&
            std::strcmp(reinterpret_cast<const char*>(e + kNameEntryString), s) == 0) {
            cache[s] = i;
            return i;
        }
    }
    return -1;
}

int32_t ObjectCount() { return *reinterpret_cast<int32_t*>(g.objectArray + 0x1c); }

UObject* ObjectAt(int32_t i) {
    if (i < 0 || i >= ObjectCount()) return nullptr;
    auto items = *reinterpret_cast<uint8_t**>(g.objectArray + 0x10);
    return *reinterpret_cast<UObject**>(items + static_cast<size_t>(i) * 0x10);
}

bool IsLive(const UObject* obj) {
    if (!Readable(obj, sizeof(UObject)) || obj->internalIndex < 0 || obj->internalIndex >= ObjectCount()) return false;
    auto items = *reinterpret_cast<uint8_t**>(g.objectArray + 0x10);
    auto item = items + static_cast<size_t>(obj->internalIndex) * 0x10;
    if (*reinterpret_cast<UObject**>(item) != obj) return false;
    return (*reinterpret_cast<int32_t*>(item + 8) & (ItemFlagPendingKill | ItemFlagUnreachable)) == 0;
}

UObject* ResolveWeak(uint64_t weak) {
    auto index = static_cast<int32_t>(weak & 0xffffffff);
    auto serial = static_cast<int32_t>(weak >> 32);
    if (serial == 0 || index < 0 || index >= ObjectCount()) return nullptr;
    auto items = *reinterpret_cast<uint8_t**>(g.objectArray + 0x10);
    if (*reinterpret_cast<int32_t*>(items + static_cast<size_t>(index) * 0x10 + 0xc) != serial) return nullptr;
    auto obj = ObjectAt(index);
    return IsLive(obj) ? obj : nullptr;
}

std::string Name(const UObject* obj) {
    if (!obj) return "None";
    std::string s = NameString(obj->name.index);
    if (obj->name.number > 0) s += "_" + std::to_string(obj->name.number - 1);
    return s;
}

std::string FullName(const UObject* obj) {
    if (!obj) return "None";
    std::string path = Name(obj);
    for (auto o = obj->outer; o; o = o->outer) path = Name(o) + "." + path;
    return Name(obj->cls) + " " + path;
}

UObject* Super(const UObject* s) {
    return *reinterpret_cast<UObject* const*>(reinterpret_cast<const uint8_t*>(s) + off::StructSuper);
}

bool IsA(const UObject* obj, const UObject* cls) {
    if (!obj || !cls) return false;
    for (auto c = obj->cls; c; c = Super(c))
        if (c == cls) return true;
    return false;
}

bool IsDefaultOrArchetype(const UObject* obj) {
    return obj->flags & (RF_ClassDefaultObject | RF_ArchetypeObject);
}

// Accepts a short object name or "Outer.Name" (e.g. "/Game/Animation/Deacon/run/run.Run").
UObject* FindObject(const char* name, const UObject* cls) {
    std::string full(name), outer;
    auto dot = full.rfind('.');
    if (dot != std::string::npos) {
        outer = full.substr(0, dot);
        full = full.substr(dot + 1);
    }
    int32_t idx = FindNameIndex(full.c_str());
    if (idx < 0) return nullptr;
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        auto o = ObjectAt(i);
        if (!o || o->name.index != idx || o->name.number != 0 || (cls && !IsA(o, cls))) continue;
        if (!outer.empty() && (!o->outer || Name(o->outer) != outer)) continue;
        return o;
    }
    return nullptr;
}

UObject* FindClass(const char* name) {
    int32_t idx = FindNameIndex(name);
    if (idx < 0) return nullptr;
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        auto o = ObjectAt(i);
        if (!o || o->name.index != idx || o->name.number != 0 || !o->cls) continue;
        std::string meta = Name(o->cls);
        if (meta.size() >= 5 && meta.compare(meta.size() - 5, 5, "Class") == 0) return o;
    }
    return nullptr;
}

UObject* FindDefaultObject(const char* className) {
    std::string n = std::string("Default__") + className;
    return FindObject(n.c_str());
}

UObject* FindChild(const UObject* s, const char* name) {
    int32_t idx = FindNameIndex(name);
    if (idx < 0) return nullptr;
    for (; s; s = Super(s)) {
        auto f = *reinterpret_cast<UObject* const*>(reinterpret_cast<const uint8_t*>(s) + off::StructChildren);
        for (; f; f = *reinterpret_cast<UObject* const*>(reinterpret_cast<const uint8_t*>(f) + off::FieldNext))
            if (f->name.index == idx) return f;
    }
    return nullptr;
}

UObject* FindFunction(const UObject* cls, const char* name) {
    auto f = FindChild(cls, name);
    return f && Name(f->cls) == "Function" ? f : nullptr;
}

int PropertyOffset(const UObject* prop) {
    return *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(prop) + g.propertyOffsetField);
}

bool DetectPropertyOffsetField(std::string& diag) {
    auto statics = FindClass("GameplayStatics");
    auto fn = statics ? FindFunction(statics, "GetPlayerPawn") : nullptr;
    if (!fn) {
        diag = "GameplayStatics.GetPlayerPawn not found";
        return false;
    }
    auto world = FindChild(fn, "WorldContextObject");
    auto index = FindChild(fn, "PlayerIndex");
    auto ret = FindChild(fn, "ReturnValue");
    if (!world || !index || !ret) {
        diag = "GetPlayerPawn parameters not found";
        return false;
    }
    for (int field = 0x40; field <= 0x74; field += 4) {
        auto at = [field](UObject* p) {
            return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(p) + field);
        };
        if (at(world) == 0 && at(index) == 8 && at(ret) == 0x10) {
            g.propertyOffsetField = field;
            diag = "UProperty::Offset_Internal at +0x" + [](int v) {
                char b[16];
                std::snprintf(b, sizeof b, "%x", v);
                return std::string(b);
            }(field);
            return true;
        }
    }
    diag = "UProperty::Offset_Internal not identified";
    return false;
}

Call::Call(UObject* target, const char* function) : target_(target), fn_(nullptr) {
    if (!IsLive(target)) return;
    fn_ = FindFunction(target->cls, function);
    if (!fn_) {
        reportError(function);
        return;
    }
    auto size = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(fn_) + off::FunctionParmsSize);
    buf_.assign(size, 0);
}

int Call::offsetOf(const char* param) const {
    if (!fn_) return -1;
    auto p = FindChild(fn_, param);
    return p ? PropertyOffset(p) : -1;
}

void Call::reportError(const char* detail) const {
    static std::unordered_set<std::string> reported;
    std::string key = Name(IsLive(target_) ? target_->cls : nullptr) + "." + Name(fn_) + ":" + detail;
    if (reported.insert(key).second)
        coop::Log("reflection: missing function or invalid parameter %s; call rejected", key.c_str());
}

int Call::checkedOffset(const char* param, size_t size) const {
    int offset = offsetOf(param);
    if (offset < 0 || static_cast<size_t>(offset) > buf_.size() ||
        size > buf_.size() - static_cast<size_t>(offset)) {
        reportError(param);
        return -1;
    }
    return offset;
}

bool Call::invoke() {
    if (!ok() || !IsLive(target_)) return false;
    g.processEvent(target_, fn_, buf_.data());
    return true;
}

} // namespace ue
