#include "research.hpp"

#include "log.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::research {

using namespace ue;

namespace {

UObject* Next(UObject* field) {
    return *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(field) + off::FieldNext);
}

UObject* Children(UObject* structure) {
    return *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(structure) + off::StructChildren);
}

int ElementSize(UObject* property) {
    return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(property) + off::PropertyElementSize);
}

bool IsFunction(UObject* field) {
    auto kind = Name(field->cls);
    return kind == "Function" || kind == "DelegateFunction";
}

std::string Signature(UObject* fn) {
    std::string text = "(";
    int count = 0;
    for (auto p = Children(fn); p && count < 64; p = Next(p), ++count) {
        if (!IsLive(p)) break;
        if (count) text += ", ";
        text += Name(p->cls) + " " + Name(p) + "@" + std::to_string(PropertyOffset(p)) + "+" +
                std::to_string(ElementSize(p));
    }
    uint32_t flags = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(fn) + off::FunctionFlags);
    char suffix[48];
    std::snprintf(suffix, sizeof suffix, ") parms=%u flags=0x%08x",
                  *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(fn) + off::FunctionParmsSize), flags);
    return text + suffix;
}

bool IsClassObject(UObject* obj) {
    if (!IsLive(obj) || !IsLive(obj->cls)) return false;
    auto kind = Name(obj->cls);
    return kind == "Class" || kind.ends_with("GeneratedClass") || kind == "ScriptStruct" ||
           kind == "UserDefinedStruct";
}

std::string Value(UObject* property, const uint8_t* data) {
    auto kind = Name(property->cls);
    int offset = PropertyOffset(property), size = ElementSize(property);
    if (offset < 0 || size <= 0) return "?";
    auto at = data + offset;
    char text[96];
    if (kind == "IntProperty" && size == 4) {
        int32_t v;
        std::memcpy(&v, at, 4);
        std::snprintf(text, sizeof text, "%d", v);
    } else if (kind == "FloatProperty" && size == 4) {
        float v;
        std::memcpy(&v, at, 4);
        std::snprintf(text, sizeof text, "%g", v);
    } else if (kind == "BoolProperty") {
        std::snprintf(text, sizeof text, "%s", *at ? "true" : "false");
    } else if (kind == "ByteProperty" && size == 1) {
        std::snprintf(text, sizeof text, "%u", *at);
    } else if (kind == "NameProperty" && size == 8) {
        FName v;
        std::memcpy(&v, at, 8);
        return "'" + NameString(v.index) + "'";
    } else if (kind == "ObjectProperty" || kind == "ClassProperty") {
        UObject* v;
        std::memcpy(&v, at, sizeof v);
        if (!v) return "null";
        return IsLive(v) ? FullName(v) : "<stale>";
    } else if (kind == "StrProperty") {
        const wchar_t* chars;
        int32_t count;
        std::memcpy(&chars, at, sizeof chars);
        std::memcpy(&count, at + sizeof chars, 4);
        if (!chars || count <= 0 || count > 256 || !ReadableMemory(chars, count * sizeof(wchar_t))) return "\"\"";
        std::string narrow;
        for (int i = 0; i + 1 < count; ++i) narrow += chars[i] < 128 ? static_cast<char>(chars[i]) : '?';
        return "\"" + narrow + "\"";
    } else if (kind == "StructProperty" && size == 12) {
        float v[3];
        std::memcpy(v, at, 12);
        std::snprintf(text, sizeof text, "(%g,%g,%g)", v[0], v[1], v[2]);
    } else {
        std::snprintf(text, sizeof text, "<%s %d>", kind.c_str(), size);
    }
    return text;
}

struct TraceEntry {
    uint64_t count = 0;
    std::string object;
    std::vector<std::string> samples;
};

struct TraceKey {
    UObject* cls;
    UObject* fn;
    bool operator==(const TraceKey&) const = default;
};

struct TraceHash {
    size_t operator()(const TraceKey& k) const {
        return std::hash<void*>()(k.cls) ^ (std::hash<void*>()(k.fn) << 1);
    }
};

bool g_tracing = false;
double g_traceStart = 0;
std::unordered_map<TraceKey, TraceEntry, TraceHash> g_trace;

double Seconds() {
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / frequency.QuadPart;
}

} // namespace

bool DumpReflection(const std::wstring& path, std::string& summary) {
    FILE* file = _wfopen(path.c_str(), L"w");
    if (!file) {
        summary = "could not open the reflection dump file";
        return false;
    }
    int classes = 0, functions = 0, properties = 0;
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        auto cls = ObjectAt(i);
        if (!IsClassObject(cls)) continue;
        ++classes;
        auto super = Super(cls);
        std::fprintf(file, "CLASS %s : %s\n", FullName(cls).c_str(), super ? Name(super).c_str() : "-");
        int budget = 0;
        for (auto field = Children(cls); field && budget < 8192; field = Next(field), ++budget) {
            if (!IsLive(field) || !IsLive(field->cls)) break;
            if (IsFunction(field)) {
                ++functions;
                std::fprintf(file, "  FN %s%s\n", Name(field).c_str(), Signature(field).c_str());
            } else {
                ++properties;
                std::fprintf(file, "  %s %s @%d+%d\n", Name(field->cls).c_str(), Name(field).c_str(),
                             PropertyOffset(field), ElementSize(field));
            }
        }
    }
    std::fclose(file);
    char text[128];
    std::snprintf(text, sizeof text, "%d classes, %d functions, %d properties", classes, functions, properties);
    summary = text;
    return true;
}

bool DumpObjects(const std::wstring& path, std::string& summary) {
    FILE* file = _wfopen(path.c_str(), L"w");
    if (!file) {
        summary = "could not open the object census file";
        return false;
    }
    std::map<std::string, int> counts;
    static const char* kInteresting[] = {"mission", "objective", "manager", "director", "spawn", "encounter",
                                         "horde", "interact", "story", "quest", "task", "ambush", "camp",
                                         "condition", "trigger", "perception", "faction", "stimul"};
    int live = 0;
    std::vector<std::string> named;
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        auto obj = ObjectAt(i);
        if (!IsLive(obj) || !IsLive(obj->cls) || IsDefaultOrArchetype(obj)) continue;
        ++live;
        auto cls = Name(obj->cls);
        ++counts[cls];
        std::string lower = cls;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        for (auto token : kInteresting) {
            if (lower.find(token) != std::string::npos) {
                if (named.size() < 50000) named.push_back(FullName(obj));
                break;
            }
        }
    }
    std::fprintf(file, "# %d live objects, %d classes\n", live, static_cast<int>(counts.size()));
    std::vector<std::pair<int, std::string>> sorted;
    for (auto& [name, count] : counts) sorted.emplace_back(count, name);
    std::sort(sorted.rbegin(), sorted.rend());
    for (auto& [count, name] : sorted) std::fprintf(file, "COUNT %6d %s\n", count, name.c_str());
    for (auto& name : named) std::fprintf(file, "OBJ %s\n", name.c_str());
    std::fclose(file);
    summary = std::to_string(live) + " live objects, " + std::to_string(named.size()) + " listed by name";
    return true;
}

void TraceStart() {
    g_trace.clear();
    g_traceStart = Seconds();
    g_tracing = true;
    Log("research: ProcessEvent trace started");
}

bool Tracing() { return g_tracing; }

void TraceEvent(UObject* obj, UObject* fn, void* parms) {
    if (!g_tracing || !obj || !fn) return;
    if (g_trace.size() >= 200000) return;
    auto& entry = g_trace[{obj->cls, fn}];
    if (entry.count++ == 0) entry.object = IsLive(obj) ? FullName(obj) : "<stale>";
    if (entry.samples.size() >= 3 || !parms || !IsLive(fn)) return;
    std::string sample;
    int count = 0;
    for (auto p = Children(fn); p && count < 24; p = Next(p), ++count) {
        if (!IsLive(p) || !IsLive(p->cls)) break;
        if (count) sample += ", ";
        sample += Name(p) + "=" + Value(p, static_cast<const uint8_t*>(parms));
    }
    if (!sample.empty()) entry.samples.push_back(std::move(sample));
}

bool TraceStop(const std::wstring& path, std::string& summary) {
    if (!g_tracing) {
        summary = "no trace is running; send trace-on first";
        return false;
    }
    g_tracing = false;
    FILE* file = _wfopen(path.c_str(), L"w");
    if (!file) {
        summary = "could not open the trace file";
        return false;
    }
    std::vector<std::pair<std::string, const TraceEntry*>> rows;
    for (auto& [key, entry] : g_trace) {
        std::string name = (IsLive(key.cls) ? Name(key.cls) : std::string("?")) + "." +
                           (IsLive(key.fn) ? Name(key.fn) : std::string("?"));
        rows.emplace_back(std::move(name), &entry);
    }
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.first < b.first; });
    std::fprintf(file, "# %.1f s, %d unique class.function pairs\n", Seconds() - g_traceStart,
                 static_cast<int>(rows.size()));
    for (auto& [name, entry] : rows) {
        std::fprintf(file, "%8llu %s  on %s\n", static_cast<unsigned long long>(entry->count), name.c_str(),
                     entry->object.c_str());
        for (auto& sample : entry->samples) std::fprintf(file, "           > %s\n", sample.c_str());
    }
    std::fclose(file);
    summary = std::to_string(rows.size()) + " unique calls traced";
    g_trace.clear();
    return true;
}

} // namespace coop::research
