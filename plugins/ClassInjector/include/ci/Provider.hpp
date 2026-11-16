#pragma once

#include "Provider.h"
#include <initializer_list>

namespace ci {
    inline EnumRename EnumRename::by_name(const char* from_name, const char* to_name) noexcept {
        return {.kind = EnumMatchKind::Name, .from_name = from_name, .to_name = to_name};
    }

    inline EnumRename EnumRename::by_value(int32_t from_value, const char* to_name) noexcept {
        return {.kind = EnumMatchKind::Value, .from_value = from_value, .to_name = to_name};
    }

    // Call-local authoring arguments only. Never retain these initializer_lists or pass
    // them across DLLs; register_enum immediately forwards their data pointers and counts.
    struct EnumChanges {
        std::initializer_list<EnumRename> renames;
        std::initializer_list<EnumMember> additions;
    };

    class Registration {
      public:
        explicit Registration(const RegistrationAPI& api) : api_(api) {}

        template <class Fn> Registration& register_func(const char* name, Fn function, uint32_t version) {
            static_assert(std::is_pointer_v<Fn> && std::is_function_v<std::remove_pointer_t<Fn>>);
            if (!api_.register_func(api_.context, name, reinterpret_cast<void*>(function), version)) {
                has_errors_ = true;
            }
            return *this;
        }

        Registration& register_enum(const char* type_name, EnumChanges changes) {
            if (!api_.register_enum ||
                !api_.register_enum(api_.context, type_name, changes.renames.begin(), changes.renames.size(),
                                    changes.additions.begin(), changes.additions.size())) {
                has_errors_ = true;
            }
            return *this;
        }

        bool has_errors() const noexcept {
            return has_errors_;
        }

      private:
        const RegistrationAPI& api_;
        bool has_errors_{};
    };
} // namespace ci

// One explicit registration entry per provider DLL; no static registry or DllMain callback.
// C++ authoring names stay local. C exports and ABI3 POD layouts remain unchanged.
#define CI_REGISTER_PROVIDER(NAME)                                                                                     \
    static void RegisterProvider(ci::Registration& NAME);                                                              \
    extern "C" __declspec(dllexport) uint32_t ci_get_sdk_abi() noexcept {                                              \
        return ci::SDK_ABI_VERSION;                                                                                    \
    }                                                                                                                  \
    extern "C" __declspec(dllexport) bool ci_register_provider(const ci::RegistrationAPI* api) {                       \
        ci::Registration registration(*api);                                                                           \
        RegisterProvider(registration);                                                                                \
        return !registration.has_errors();                                                                             \
    }                                                                                                                  \
    static void RegisterProvider(ci::Registration& NAME)
