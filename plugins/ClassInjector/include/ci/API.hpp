#pragma once
#include "API.h"
#include <cstring>
#include <stdexcept>
#include <vector>

namespace ci {
    // Each view carries its host table. No process-global SDK singleton or owning wrappers.
    class API {
      public:
        struct TypeDefinition;
        struct Field;
        struct Method;
        struct TDB;
        struct Objects;
        struct Resources;

        struct ManagedObject {
            const SDKAPI* sdk{};
            void* handle{};

            template <class T> T* get_field(const char* name) const {
                if (!handle)
                    return nullptr;
                const auto type = sdk->managed_object->get_type_definition(handle);
                const auto field = sdk->type_definition->find_field(type, name);
                return field ? static_cast<T*>(sdk->field->get_data_raw(field, handle, false)) : nullptr;
            }
        };

        explicit API(const SDKAPI& sdk) : sdk_(&sdk) {}

        TDB tdb() const;
        Objects objects() const;
        Resources resources() const;

        struct Field {
            const SDKAPI* sdk{};
            FieldHandle handle{};

            explicit operator bool() const {
                return handle != nullptr;
            }

            const char* get_name() const {
                return sdk->field->get_name(handle);
            }

            TypeDefinition get_type() const;
            TypeDefinition get_declaring_type() const;

            uint32_t get_flags() const {
                return sdk->field->get_flags(handle);
            }

            bool is_static() const {
                return sdk->field->is_static(handle);
            }

            bool is_literal() const {
                return sdk->field->is_literal(handle);
            }

            uint32_t get_offset_from_base() const {
                return sdk->field->get_offset_from_base(handle);
            }

            uint32_t get_offset_from_fieldptr() const {
                return sdk->field->get_offset_from_fieldptr(handle);
            }

            void* get_init_data() const {
                return sdk->field->get_init_data(handle);
            }

            void* get_data_raw(void* object, bool is_value_type = false) const {
                return sdk->field->get_data_raw(handle, object, is_value_type);
            }

            template <class T> T& get_data(void* object = nullptr, bool is_value_type = false) const {
                return *static_cast<T*>(get_data_raw(object, is_value_type));
            }
        };

        struct Method {
            const SDKAPI* sdk{};
            MethodHandle handle{};

            explicit operator bool() const {
                return handle != nullptr;
            }

            const char* get_name() const {
                return sdk->method->get_name(handle);
            }

            template <class Fn = void*> Fn get_function() const {
                static_assert(std::is_pointer_v<Fn>);
                return reinterpret_cast<Fn>(sdk->method->get_function(handle));
            }

            bool is_static() const {
                return sdk->method->is_static(handle);
            }
        };

        struct TypeDefinition {
            const SDKAPI* sdk{};
            TypeDefinitionHandle handle{};

            explicit operator bool() const {
                return handle != nullptr;
            }

            const char* get_full_name() const {
                return sdk->type_definition->get_full_name(handle);
            }

            TypeDefinition get_parent_type() const {
                return {sdk, sdk->type_definition->get_parent_type(handle)};
            }

            TypeDefinition get_element_type() const {
                return {sdk, sdk->type_definition->get_element_type(handle)};
            }

            bool is_enum() const {
                return sdk->type_definition->is_enum(handle);
            }

            bool is_valuetype() const {
                return sdk->type_definition->is_valuetype(handle);
            }

            TypeDefinition get_underlying_type() const {
                return {sdk, sdk->type_definition->get_underlying_type(handle)};
            }

            uint32_t get_index() const {
                return sdk->type_definition->get_index(handle);
            }

            uint32_t get_num_fields() const {
                return sdk->type_definition->get_num_fields(handle);
            }

            uint32_t get_num_methods() const {
                return sdk->type_definition->get_num_methods(handle);
            }

            uint32_t get_array_rank() const {
                return sdk->type_definition->get_array_rank(handle);
            }

            bool is_derived_from(TypeDefinition base) const {
                return sdk->type_definition->is_derived_from(handle, base.handle);
            }

            bool is_array() const {
                return sdk->type_definition->is_array(handle);
            }

            uint32_t get_valuetype_size() const {
                return sdk->type_definition->get_valuetype_size(handle);
            }

            Field find_field(const char* name) const {
                return {sdk, sdk->type_definition->find_field(handle, name)};
            }

            Method find_method(const char* name) const {
                return {sdk, sdk->type_definition->find_method(handle, name)};
            }

            std::vector<Field> get_fields() const {
                std::vector<FieldHandle> handles(sdk->type_definition->get_num_fields(handle));
                if (sdk->type_definition->get_fields(handle, handles.data(),
                                                     static_cast<uint32_t>(handles.size() * sizeof(FieldHandle)),
                                                     nullptr) != ERROR_NONE)
                    throw std::runtime_error("CI get_fields failed");
                std::vector<Field> result;
                result.reserve(handles.size());
                for (auto field : handles)
                    result.push_back({sdk, field});
                return result;
            }

            std::vector<Method> get_methods() const {
                std::vector<MethodHandle> handles(sdk->type_definition->get_num_methods(handle));
                if (sdk->type_definition->get_methods(handle, handles.data(),
                                                      static_cast<uint32_t>(handles.size() * sizeof(MethodHandle)),
                                                      nullptr) != ERROR_NONE)
                    throw std::runtime_error("CI get_methods failed");
                std::vector<Method> result;
                result.reserve(handles.size());
                for (auto method : handles)
                    result.push_back({sdk, method});
                return result;
            }
        };

        struct TDB {
            const SDKAPI* sdk{};

            TypeDefinition find_type(const char* name) const {
                return {sdk, sdk->tdb->find_type(name)};
            }
        };

        struct Objects {
            const SDKAPI* sdk{};

            void* create_instance(void* vmContext, TypeDefinition type, bool allocateOnHeap = false) const {
                return sdk->managed_object->create_instance(vmContext, type.handle, allocateOnHeap);
            }

            void* create_array(void* vmContext, TypeDefinition elementType, uint64_t count,
                               bool allocateOnHeap = false) const {
                return sdk->managed_object->create_array(vmContext, elementType.handle, count, allocateOnHeap);
            }

            void* create_action(void* vmContext, TypeDefinition actionType, void* target, Method method,
                                bool allocateOnHeap = false) const {
                return sdk->managed_object->create_action(vmContext, actionType.handle, target, method.handle,
                                                          allocateOnHeap);
            }

            void* create_string(void* vmContext, const char* text) const {
                return sdk->managed_object->create_string(vmContext, text);
            }

            bool assign_reference(void* owner, void** field, void* value) const {
                return sdk->managed_object->assign_reference(owner, field, value);
            }

            void retain(void* object) const {
                sdk->managed_object->add_ref(object);
            }

            void release(void* object) const {
                sdk->managed_object->release(object);
            }
        };

        struct Resources {
            const SDKAPI* sdk{};

            void* create_userdata(TypeDefinition type, const char* uri) const {
                return sdk->resource_manager->create_userdata(type.handle, uri);
            }

            void* create_resource_holder(void* vmContext, TypeDefinition type, const char* path) const {
                return sdk->resource_manager->create_resource_holder(vmContext, type.handle, path);
            }

            uint32_t deserialize_rsz(void* vmContext, RszObjectArray* roots, const uint8_t* data, size_t size) const {
                return sdk->resource_manager->deserialize_rsz(vmContext, roots, data, size);
            }
        };

      private:
        const SDKAPI* sdk_;
    };

    inline API::TDB API::tdb() const {
        return {sdk_};
    }

    inline API::Objects API::objects() const {
        return {sdk_};
    }

    inline API::Resources API::resources() const {
        return {sdk_};
    }

    inline API::TypeDefinition API::Field::get_type() const {
        return {sdk, sdk->field->get_type(handle)};
    }

    inline API::TypeDefinition API::Field::get_declaring_type() const {
        return {sdk, sdk->field->get_declaring_type(handle)};
    }
} // namespace ci
