// Minimal runtime reflection for Days Gone's Unreal Engine 4.10 fork.
// Layouts were verified against Steam build 19221447 (see README).
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ue {

struct FName { int32_t index; int32_t number; };
struct FVector { float x, y, z; };
struct FRotator { float pitch, yaw, roll; };
struct alignas(16) FQuat { float x, y, z, w; };
struct alignas(16) FTransform { FQuat rotation; FVector translation; float pad0; FVector scale; float pad1; };
struct FIntVector { int32_t x, y, z; };

struct UObject {
    void** vtable;
    int32_t flags;
    int32_t internalIndex;
    UObject* cls;
    FName name;
    UObject* outer;
};

// Field offsets inside reflection objects (UE 4.10 layout, verified in ProcessEvent).
namespace off {
constexpr int FieldNext = 0x28;
constexpr int StructSuper = 0x30;
constexpr int StructChildren = 0x38;
constexpr int FunctionFlags = 0x88;
constexpr int FunctionParmsSize = 0x90;
constexpr int PropertyArrayDim = 0x30;
constexpr int PropertyElementSize = 0x34;
}

constexpr int32_t RF_ClassDefaultObject = 0x10;
constexpr int32_t RF_ArchetypeObject = 0x20;
constexpr int32_t ItemFlagUnreachable = 1 << 28;
constexpr int32_t ItemFlagPendingKill = 1 << 29;

using ProcessEventFn = void (*)(UObject*, UObject*, void*);

struct Globals {
    uintptr_t imageBase = 0;
    uint8_t* objectArray = nullptr;        // FUObjectArray
    uint8_t* names = nullptr;              // TNameEntryArray
    ProcessEventFn processEvent = nullptr; // trampoline to the original
    int propertyOffsetField = -1;          // offset of UProperty::Offset_Internal
};
extern Globals g;

bool InitNames(std::string& diag);
bool DetectPropertyOffsetField(std::string& diag);

int32_t ObjectCount();
UObject* ObjectAt(int32_t i);
bool IsLive(const UObject* obj);
// Resolves a TWeakObjectPtr (ObjectIndex, SerialNumber) to a live object or nullptr.
UObject* ResolveWeak(uint64_t weak);

std::string NameString(int32_t index);
std::string Name(const UObject* obj);
std::string FullName(const UObject* obj);
int32_t FindNameIndex(const char* s);

UObject* Super(const UObject* structObj);
bool IsA(const UObject* obj, const UObject* cls);
bool IsDefaultOrArchetype(const UObject* obj);

UObject* FindClass(const char* name);
UObject* FindObject(const char* name, const UObject* cls = nullptr);
UObject* FindDefaultObject(const char* className);
UObject* FindChild(const UObject* structObj, const char* name);
UObject* FindFunction(const UObject* cls, const char* name);
int PropertyOffset(const UObject* prop);

template <typename T> bool GetProp(UObject* obj, const char* name, T& out) {
    UObject* p = FindChild(obj->cls, name);
    if (!p) return false;
    std::memcpy(&out, reinterpret_cast<uint8_t*>(obj) + PropertyOffset(p), sizeof(T));
    return true;
}

// Builds a parameter buffer for a UFunction and calls it through ProcessEvent.
class Call {
public:
    Call(UObject* target, const char* function);
    bool ok() const { return valid_ && fn_ != nullptr && target_ != nullptr; }
    template <typename T> Call& set(const char* param, const T& v) {
        if (int o = checkedOffset(param, sizeof(T)); o >= 0)
            std::memcpy(buf_.data() + o, &v, sizeof(T));
        else
            valid_ = false;
        return *this;
    }
    Call& setBool(const char* param, bool v) {
        return set(param, static_cast<uint8_t>(v ? 1 : 0));
    }
    bool invoke();
    template <typename T> T get(const char* param) const {
        T v{};
        if (int o = checkedOffset(param, sizeof(T)); o >= 0)
            std::memcpy(&v, buf_.data() + o, sizeof(T));
        return v;
    }
    bool has(const char* param) const { return offsetOf(param) >= 0; }

private:
    int offsetOf(const char* param) const;
    int checkedOffset(const char* param, size_t size) const;
    void reportError(const char* detail) const;
    UObject* target_;
    UObject* fn_;
    std::vector<uint8_t> buf_;
    bool valid_ = true;
};

} // namespace ue
