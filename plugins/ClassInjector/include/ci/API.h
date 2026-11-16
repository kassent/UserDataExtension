#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ci {
    struct TypeDefinitionHandleTag;
    struct FieldHandleTag;
    struct MethodHandleTag;
    using TypeDefinitionHandle = const TypeDefinitionHandleTag*;
    using FieldHandle = const FieldHandleTag*;
    using MethodHandle = const MethodHandleTag*;
    // REF result convention, not a new general-purpose error taxonomy.
    using Result = int;
    inline constexpr Result ERROR_UNKNOWN = -1;
    inline constexpr Result ERROR_NONE = 0;
    inline constexpr Result ERROR_OUT_TOO_SMALL = 1;
    inline constexpr Result ERROR_EXCEPTION = 2;

    struct TDB {
        TypeDefinitionHandle (*find_type)(const char* name);
    };

    struct TDBTypeDefinition {
        const char* (*get_full_name)(TypeDefinitionHandle type);
        TypeDefinitionHandle (*get_parent_type)(TypeDefinitionHandle type);
        TypeDefinitionHandle (*get_element_type)(TypeDefinitionHandle type);
        bool (*is_enum)(TypeDefinitionHandle type);
        bool (*is_valuetype)(TypeDefinitionHandle type);
        bool (*is_array)(TypeDefinitionHandle type);
        uint32_t (*get_valuetype_size)(TypeDefinitionHandle type);
        uint32_t (*get_num_fields)(TypeDefinitionHandle type);
        uint32_t (*get_num_methods)(TypeDefinitionHandle type);
        FieldHandle (*find_field)(TypeDefinitionHandle type, const char* name);
        MethodHandle (*find_method)(TypeDefinitionHandle type, const char* signature);
        bool (*is_derived_from)(TypeDefinitionHandle type, TypeDefinitionHandle baseType);
        uint32_t (*get_index)(TypeDefinitionHandle type);
        uint32_t (*get_array_rank)(TypeDefinitionHandle type);
        TypeDefinitionHandle (*get_underlying_type)(TypeDefinitionHandle type);
        // Like REF: out_size is bytes; out_count is number of handles written.
        Result (*get_fields)(TypeDefinitionHandle type, FieldHandle* out_fields, uint32_t out_size,
                             uint32_t* out_count);
        Result (*get_methods)(TypeDefinitionHandle type, MethodHandle* out_methods, uint32_t out_size,
                              uint32_t* out_count);
    };

    struct TDBField {
        const char* (*get_name)(FieldHandle field);
        TypeDefinitionHandle (*get_type)(FieldHandle field);
        TypeDefinitionHandle (*get_declaring_type)(FieldHandle field);
        uint32_t (*get_flags)(FieldHandle field);
        bool (*is_static)(FieldHandle field);
        bool (*is_literal)(FieldHandle field);
        uint32_t (*get_offset_from_base)(FieldHandle field);
        uint32_t (*get_offset_from_fieldptr)(FieldHandle field);
        // Borrowed metadata initializer; never executes cctor. No data -> nullptr.
        void* (*get_init_data)(FieldHandle field);
        // Returns a slot, not the referenced object. Reference writes need assign_reference.
        // is_value_type selects unboxed payload addressing. Nonliteral static storage is not exposed.
        void* (*get_data_raw)(FieldHandle field, void* object, bool is_value_type);
    };

    struct TDBMethod {
        const char* (*get_name)(MethodHandle method);
        void* (*get_function)(MethodHandle method);
        bool (*is_static)(MethodHandle method);
    };

    struct RszObjectArray {
        void** data;
        uint32_t count;
        uint32_t capacity;
    };

    struct ManagedObject {
        TypeDefinitionHandle (*get_type_definition)(const void* object) noexcept;
        // Real native managed scope required. Allocation does not call .ctor or retain.
        void* (*create_instance)(void* vmContext, TypeDefinitionHandle type, bool allocateOnHeap);
        void* (*create_array)(void* vmContext, TypeDefinitionHandle elementType, uint64_t count, bool allocateOnHeap);
        void (*add_ref)(void* object);
        void (*release)(void* object);
        bool (*assign_reference)(void* owner, void** field, void* value) noexcept;
        void* (*create_string)(void* vmContext, const char* text);
        void* (*create_action)(void* vmContext, TypeDefinitionHandle actionType, void* target, MethodHandle method,
                               bool allocateOnHeap);
    };

    struct ResourceManager {
        // One owned managed reference; fields may not yet be decoded.
        void* (*create_userdata)(TypeDefinitionHandle type, const char* uri);
        // Holder owns the native resource request; no extra managed retain.
        void* (*create_resource_holder)(void* vmContext, TypeDefinitionHandle holderType, const char* path);
        uint32_t (*deserialize_rsz)(void* vmContext, RszObjectArray* roots, const uint8_t* data, size_t size);
    };

    struct SDKAPI {
        const TDB* tdb;
        const TDBTypeDefinition* type_definition;
        const TDBField* field;
        const TDBMethod* method;
        const struct ManagedObject* managed_object{};
        const struct ResourceManager* resource_manager{};
    };

    static_assert(sizeof(Result) == 4);
    static_assert(sizeof(RszObjectArray) == 16 && offsetof(RszObjectArray, count) == 8);
    static_assert(sizeof(SDKAPI) == 6 * sizeof(void*));
    static_assert(std::is_standard_layout_v<SDKAPI> && std::is_trivially_copyable_v<SDKAPI>);
} // namespace ci
