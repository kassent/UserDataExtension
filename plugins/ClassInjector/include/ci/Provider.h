#pragma once

#include "API.h"
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ci {
    // Export names, layouts, signatures and POD semantics share this binary SDK identity.
    // Independent of TOML schema_version and individual function binding versions.
    inline constexpr uint32_t SDK_ABI_VERSION = 3;
    using FnGetSDKABI = uint32_t(__cdecl*)() noexcept;

    struct ProviderContext {
        const SDKAPI* sdk;
    };

    // Called after native entries and custom virtual tables are ready, before global cctors:
    // all ci_on_initialize exports, then all ci_on_post_initialize exports.
    // Neither phase means REFramework, game singletons or resources are initialized.
    // Optional services may be null. Runtime operations require a real native managed scope;
    // obtaining a VM context or calling from bare IPC/UpdateBehavior does not create one.
    using FnInitializationCallback = void(__cdecl*)(const ProviderContext* context) noexcept;

    enum class EnumMatchKind : uint32_t { Name, Value };

    struct EnumRename {
        EnumMatchKind kind;

        union {
            const char* from_name;
            int32_t from_value;
        };

        const char* to_name;

        // Explicit selectors: zero is a valid value, never an unset marker.
        // Local construction helpers are defined in Provider.hpp.
        static EnumRename by_name(const char* from_name, const char* to_name) noexcept;
        static EnumRename by_value(int32_t from_value, const char* to_name) noexcept;
    };

    struct EnumMember {
        const char* name;
        int32_t value;
    };

    using FnRegisterFunc = bool(__cdecl*)(void* context, const char* name, void* address, uint32_t version);
    using FnRegisterEnum = bool(__cdecl*)(void* context, const char* type_name, const EnumRename* renames,
                                          size_t rename_count, const EnumMember* additions, size_t addition_count);

    // Host callbacks copy inputs during each call into this provider's pending declarations.
    // No VM or REFramework SDK access, owning C++ objects or STL cross this boundary.
    struct RegistrationAPI {
        void* context;
        FnRegisterFunc register_func;
        FnRegisterEnum register_enum{};
    };

    using FnRegisterProvider = bool(__cdecl*)(const RegistrationAPI* api);

    // ABI3 keeps the original x64 POD layouts despite the source-level API reorganization.
    static_assert(std::is_standard_layout_v<ProviderContext> && std::is_trivially_copyable_v<ProviderContext>);
    static_assert(sizeof(ProviderContext) == sizeof(void*));
    static_assert(std::is_standard_layout_v<EnumRename> && std::is_trivial_v<EnumRename>);
    static_assert(offsetof(EnumRename, kind) == 0 && offsetof(EnumRename, from_name) == sizeof(void*));
    static_assert(offsetof(EnumRename, from_value) == sizeof(void*) &&
                  offsetof(EnumRename, to_name) == 2 * sizeof(void*));
    static_assert(sizeof(EnumRename) == 3 * sizeof(void*));
    static_assert(std::is_standard_layout_v<EnumMember> && std::is_trivially_copyable_v<EnumMember>);
    static_assert(offsetof(EnumMember, name) == 0 && offsetof(EnumMember, value) == sizeof(void*));
    static_assert(sizeof(EnumMember) == 2 * sizeof(void*));
    static_assert(std::is_standard_layout_v<RegistrationAPI> && std::is_trivially_copyable_v<RegistrationAPI>);
    static_assert(offsetof(RegistrationAPI, context) == 0 && offsetof(RegistrationAPI, register_func) == sizeof(void*));
    static_assert(offsetof(RegistrationAPI, register_enum) == 2 * sizeof(void*));
    static_assert(sizeof(RegistrationAPI) == 3 * sizeof(void*));
} // namespace ci
