#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <type_traits>

namespace RE {
    struct RETypeDefinition;
}

namespace ci {
    // Binary SDK contract; increment for layout, signature, or POD semantic
    // changes. Independent of TOML schema_version and individual function binding
    // versions.
    inline constexpr uint32_t SDK_ABI_VERSION = 1;
    using FnGetSdkAbi = uint32_t(__cdecl*)() noexcept;

    // Borrowed published TDB handle for native and custom types; host lifetime, no
    // REFramework SDK dependency.
    using Type = RE::RETypeDefinition;
    using FnGetType = const Type*(__cdecl*)(const char* fullName);
    using FnGetMethod = void*(__cdecl*)(const Type* type, const char* signature);
    using FnIsDerivedFrom = bool(__cdecl*)(const Type* type, const Type* baseType);
    using FnIsEnum = bool(__cdecl*)(const Type* type);
    using FnGetValueTypeSize = uint32_t(__cdecl*)(const Type* type);

    // Field metadata: available to native loaders before REFramework SDK
    // initialization.
    struct FieldLayout {
        const Type* type;
        uint32_t valueOffset;  // Offset within an unboxed value.
        uint32_t objectOffset; // Default layout; instance access uses the object header field base.
        uint32_t valueSize;    // Zero for a reference; otherwise the native value payload size.
    };
    // Stable host-owned layout, or nullptr when absent. The caller must not release
    // it.
    using FnGetFieldLayout = const FieldLayout*(__cdecl*)(const Type* type, const char* name);

    // Managed objects cross the DLL boundary as borrowed void* handles, independent
    // of REFramework SDK types.
    using FnGetInstanceType = const Type*(__cdecl*)(const void* object) noexcept;
    using FnGetRawField = void*(__cdecl*)(void* object, const FieldLayout* field) noexcept;
    using FnCreateInstance = void*(__cdecl*)(void* vmctx, const Type* type, bool forceHeapAllocation);
    using FnCreateArray = void*(__cdecl*)(void* vmctx, const Type* elementType, uint64_t length,
                                          bool forceHeapAllocation);
    // Method lookup follows GetMethod; target may be null for a static method.
    // forceHeapAllocation=false permits temporary VM arena allocation;
    // publish/retain within a real managed scope.
    using FnCreateAction = void*(__cdecl*)(void* vmctx, const Type* actionType, void* target, const Type* methodType,
                                           const char* signature, bool forceHeapAllocation);
    using FnRetain = void(__cdecl*)(void* object);
    using FnRelease = void(__cdecl*)(void* object);
    using FnAssignReference = bool(__cdecl*)(void* owner, void** field, void* value) noexcept;
    using FnCreateString = void*(__cdecl*)(void* vmctx, const char* utf8);

    // Native RSZ input/output array. The reader may replace data; inspect the
    // returned array.
    struct RszObjectArray {
        void** data;
        uint32_t count;
        uint32_t capacity;
    };
    using FnDeserializeRsz = uint32_t(__cdecl*)(void* vmctx, RszObjectArray* objects, const uint8_t* data, size_t size);
    using FnCreateUserData = void*(__cdecl*)(const Type* type, const char* utf8Path);
    using FnCreateResourceHolder = void*(__cdecl*)(void* vmctx, const Type* type, const char* utf8Path);

    // Host-owned services, valid until process exit and populated before the ready notification.
    // Optional unresolved services are null. vmctx must originate in a real native managed call scope;
    // this table does not establish one. forceHeapAllocation=false permits temporary arena allocation.
    struct ProviderContext {
        FnGetType GetType;
        // Bare name or Name(Type1, Type2); a bare name selects the first non-open-generic declaration.
        // Exact signatures use element types for ref/out/pointers. No object virtual dispatch.
        FnGetMethod GetMethod;
        FnGetFieldLayout GetFieldLayout{};
        FnGetInstanceType GetInstanceType{};
        FnGetRawField GetRawField{};
        // Allocates without calling .ctor or retaining.
        FnCreateInstance CreateInstance{};
        // Fill before first retain/publication; value arrays are supported.
        FnCreateArray CreateArray{};
        // Native reference operations require a non-null object.
        FnRetain Retain{};
        FnRelease Release{};
        // Retain/CAS/release; available only when both reference services are resolved.
        FnAssignReference AssignReference{};
        // NUL-terminated UTF-8 to UTF-16; null input returns nullptr; no extra retain.
        FnCreateString CreateString{};
        // Returns native status; each output root owns one reference.
        FnDeserializeRsz DeserializeRsz{};
        // Creates a System.Action delegate bound to methodType/signature.
        FnCreateAction CreateAction{};
        // Requests a native USER resource with one owned reference; fields may not be decoded yet.
        FnCreateUserData CreateUserData{};
        // Creates a holder owning a native resource reference; no extra managed retain.
        FnCreateResourceHolder CreateResourceHolder{};
        // Same type or parent chain only; no interfaces, array covariance, or generic variance.
        FnIsDerivedFrom IsDerivedFrom{};
        // Direct parent is System.Enum; excludes System.Enum itself.
        FnIsEnum IsEnum{};
        // Closed value payload size; zero for a non-value type or missing layout.
        FnGetValueTypeSize GetValueTypeSize{};

        // Returns the field slot T*, without dereferencing it or checking T.
        // T=Object* returns Object**. Reference writes still require AssignReference.
        template <class T = void>
        T* GetField(void* object, const FieldLayout* field) const noexcept {
            return GetRawField ? static_cast<T*>(GetRawField(object, field)) : nullptr;
        }

        template <class T>
        T* GetField(void* object, const char* name) const {
            if (!object || !name || !GetInstanceType || !GetFieldLayout || !GetRawField)
                return nullptr;
            const auto* type = GetInstanceType(object);
            return type ? GetField<T>(object, GetFieldLayout(type, name)) : nullptr;
        }
    };
    static_assert(std::is_standard_layout_v<ProviderContext> && std::is_trivially_copyable_v<ProviderContext>);
    static_assert(offsetof(ProviderContext, GetType) == 0 && offsetof(ProviderContext, GetMethod) == sizeof(void*));
    static_assert(offsetof(ProviderContext, GetFieldLayout) == 2 * sizeof(void*) &&
                  offsetof(ProviderContext, GetRawField) == 4 * sizeof(void*) &&
                  offsetof(ProviderContext, CreateArray) == 6 * sizeof(void*) &&
                  offsetof(ProviderContext, CreateAction) == 12 * sizeof(void*) &&
                  offsetof(ProviderContext, CreateUserData) == 13 * sizeof(void*) &&
                  offsetof(ProviderContext, CreateResourceHolder) == 14 * sizeof(void*));
    static_assert(offsetof(ProviderContext, IsDerivedFrom) == 15 * sizeof(void*) &&
                  offsetof(ProviderContext, IsEnum) == 16 * sizeof(void*));
    static_assert(offsetof(ProviderContext, GetValueTypeSize) == 17 * sizeof(void*));
    static_assert(sizeof(ProviderContext) == 18 * sizeof(void*));
    static_assert(sizeof(RszObjectArray) == 16 && offsetof(RszObjectArray, count) == 8 &&
                  offsetof(RszObjectArray, capacity) == 12);

    // Unresolved optional services are null; metadata-only providers may still
    // operate. ci_on_tdb_ready runs once after native entries and custom virtual
    // tables are ready, before global static constructors. Business static objects
    // may not exist yet; no managed scope creation, constructor replay, or
    // REFramework SDK dependency. vmctx must originate in a real native lifecycle;
    // bare IPC/UpdateBehavior calls do not establish that scope.
    using FnTdbReady = void(__cdecl*)(const ProviderContext* context) noexcept;

    // Provider declarations and registration.
    // Only POD records and function pointers cross the DLL boundary. Registration
    // does not access the VM or REFramework SDK.
    enum class EnumRenameSource : uint32_t { Name, Value };
    struct EnumRename {
        EnumRenameSource source;
        union {
            const char* fromName;
            int32_t fromValue;
        };
        const char* to;

        // Explicitly select the source: zero is a valid value, not an unset marker.
        static EnumRename FromName(const char* name, const char* to) noexcept {
            return {.source = EnumRenameSource::Name, .fromName = name, .to = to};
        }
        static EnumRename FromValue(int32_t value, const char* to) noexcept {
            return {.source = EnumRenameSource::Value, .fromValue = value, .to = to};
        }
    };
    struct EnumAppend {
        const char* name;
        int32_t value;
    };
    using FnAddFunction = bool(__cdecl*)(void* context, const char* name, void* address, uint32_t version);
    using FnAddEnumExtension = bool(__cdecl*)(void* context, const char* type, const EnumRename* renames,
                                              size_t renameCount, const EnumAppend* appends, size_t appendCount);
    struct ProviderRegistrationApi {
        void* context;
        FnAddFunction AddFunction;
        // Inputs are copied into the current provider during this call.
        FnAddEnumExtension AddEnumExtension{};
    };
    static_assert(std::is_standard_layout_v<EnumRename> && std::is_trivially_copyable_v<EnumRename>);
    static_assert(std::is_trivial_v<EnumRename>);
    static_assert(offsetof(EnumRename, source) == 0 && offsetof(EnumRename, fromName) == sizeof(void*));
    static_assert(offsetof(EnumRename, fromValue) == sizeof(void*) && offsetof(EnumRename, to) == 2 * sizeof(void*));
    static_assert(sizeof(EnumRename) == 3 * sizeof(void*));
    static_assert(std::is_standard_layout_v<EnumAppend> && std::is_trivially_copyable_v<EnumAppend>);
    static_assert(std::is_standard_layout_v<ProviderRegistrationApi> &&
                  std::is_trivially_copyable_v<ProviderRegistrationApi>);
    static_assert(offsetof(ProviderRegistrationApi, context) == 0 &&
                  offsetof(ProviderRegistrationApi, AddFunction) == sizeof(void*));
    static_assert(offsetof(ProviderRegistrationApi, AddEnumExtension) == 2 * sizeof(void*));
    static_assert(sizeof(ProviderRegistrationApi) == 3 * sizeof(void*));

    // C++ authoring interface: initializer_list stays inside the provider, not
    // across the DLL boundary. Registers native implementations and enum edits;
    // managed signatures remain explicit in TOML. Explicit .ctor implementations
    // call their own base constructor; CI adds no clearing or base call.
    class ProviderRegistration {
      public:
        explicit ProviderRegistration(const ProviderRegistrationApi& api) : api_(api) {}

        template <class Fn>
        ProviderRegistration& RegisterFunction(const char* name, Fn function, uint32_t version) {
            static_assert(std::is_pointer_v<Fn> && std::is_function_v<std::remove_pointer_t<Fn>>);
            if (!api_.AddFunction(api_.context, name, reinterpret_cast<void*>(function), version)) {
                succeeded_ = false;
            }
            return *this;
        }

        bool HasSucceeded() const { return succeeded_; }

        ProviderRegistration& ExtendEnum(const char* type, std::initializer_list<EnumRename> renames,
                                         std::initializer_list<EnumAppend> appends) {
            if (!api_.AddEnumExtension || !api_.AddEnumExtension(api_.context, type, renames.begin(), renames.size(),
                                                                 appends.begin(), appends.size())) {
                succeeded_ = false;
            }
            return *this;
        }

      private:
        const ProviderRegistrationApi& api_;
        bool succeeded_{true};
    };

    using FnRegisterProvider = bool(__cdecl*)(const ProviderRegistrationApi* api);
} // namespace ci

// Use once in one .cpp per provider DLL; exports the ABI and registration
// entry. Implementations need no dllexport. Exports use stable C symbols; C++
// authoring names are not part of the binary layout. Registration is called
// explicitly; no cross-module static registry or DllMain callback.
#define CI_REGISTER_PROVIDER(NAME)                                                                                     \
    static void RegisterProvider(ci::ProviderRegistration& NAME);                                                      \
    extern "C" __declspec(dllexport) uint32_t ci_get_sdk_abi() noexcept {                                              \
        return ci::SDK_ABI_VERSION;                                                                                    \
    }                                                                                                                  \
    extern "C" __declspec(dllexport) bool ci_register_provider(const ci::ProviderRegistrationApi* api) {               \
        ci::ProviderRegistration registration(*api);                                                                   \
        RegisterProvider(registration);                                                                                \
        return registration.HasSucceeded();                                                                            \
    }                                                                                                                  \
    static void RegisterProvider(ci::ProviderRegistration& NAME)
