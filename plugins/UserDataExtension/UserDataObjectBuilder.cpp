#include "UserDataObjectBuilder.h"
#include "UserDataCollection.h"
#include <GameTypes.h>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace UserDataExtension {
using Json = nlohmann::json;
using Fnset_Standby = void (*)(void*, void*, bool);

// 固定元数据在 CI-ready 后首次使用时缓存，随已发布 TDB 保持进程期有效。
// JSON 指定的类型和实际对象字段仍按本次声明/实例查询，不共用首次结果。

// 判断受支持整数或枚举是否按无符号 payload 处理；Char 按 UInt16 语义处理。
// 枚举取 CLR underlying element 编码，不把该编码误当成 TDB 类型 ID。
bool IsUnsignedInteger(const ci::ProviderContext& context, const ci::Type* type) {
    if (context.IsEnum(type)) {
        const auto elementType = static_cast<via::clr::ElementType>(type->underlyingTypeIndex);
        return elementType == via::clr::ElementType::U1 || elementType == via::clr::ElementType::U2 || elementType == via::clr::ElementType::U4 || elementType == via::clr::ElementType::U8;
    }
    static const ci::Type* const unsignedTypes[]{context.GetType("System.Byte"), context.GetType("System.UInt16"), context.GetType("System.UInt32"), context.GetType("System.UInt64"), context.GetType("System.Char")};
    for (const auto* unsignedType : unsignedTypes)
        if (type == unsignedType)
            return true;
    return false;
}

// 返回 v1 支持的基础标量、枚举或 Guid 的 payload 字节数，用于字段和集合布局检查。
// 复杂结构体不猜步长；未知标量或枚举底层编码直接抛异常。
uint32_t GetScalarSize(const ci::ProviderContext& context, const ci::Type* type) {
    if (!type)
        throw std::runtime_error("value type is missing");
    static const std::pair<const ci::Type*, size_t> scalarTypes[]{
        {context.GetType("System.Guid"), sizeof(RE::SystemGuid)},
        {context.GetType("System.Boolean"), sizeof(bool)},
        {context.GetType("System.Byte"), sizeof(uint8_t)},
        {context.GetType("System.SByte"), sizeof(int8_t)},
        {context.GetType("System.Int16"), sizeof(int16_t)},
        {context.GetType("System.UInt16"), sizeof(uint16_t)},
        {context.GetType("System.Char"), sizeof(char16_t)},
        {context.GetType("System.Int32"), sizeof(int32_t)},
        {context.GetType("System.UInt32"), sizeof(uint32_t)},
        {context.GetType("System.Single"), sizeof(float)},
        {context.GetType("System.Int64"), sizeof(int64_t)},
        {context.GetType("System.UInt64"), sizeof(uint64_t)},
        {context.GetType("System.Double"), sizeof(double)}};
    for (const auto& [scalarType, size] : scalarTypes)
        if (type == scalarType)
            return uint32_t(size);
    if (context.IsEnum(type)) {
        // I/U 分别表示有符号/无符号整数，后缀是字节宽度；不可用该编码索引类型表。
        switch (static_cast<via::clr::ElementType>(type->underlyingTypeIndex)) {
        case via::clr::ElementType::I1:
        case via::clr::ElementType::U1:
            return sizeof(int8_t);
        case via::clr::ElementType::I2:
        case via::clr::ElementType::U2:
            return sizeof(int16_t);
        case via::clr::ElementType::I4:
        case via::clr::ElementType::U4:
            return sizeof(int32_t);
        case via::clr::ElementType::I8:
        case via::clr::ElementType::U8:
            return sizeof(int64_t);
        }
    }
    throw std::runtime_error("unsupported scalar/array element layout");
}

// 首次使用时绑定少量高频纯值类型；仅对命中的类型检查布局，不影响未使用的可选类型。
// 描述表是支持政策，实际 payload/步长仍由 CI 元数据决定，不把任意小结构当成纯值。
const SupportedValueLayout* FindSupportedValueLayout(const ci::ProviderContext& context, const ci::Type* type) {
    struct Description {
        const char* name;
        std::array<const char*, 4> members;
        bool nullable{};
    };
    static const auto layouts = [&context] {
        constexpr Description descriptions[]{
            {.name = "via.vec2", .members = {"x", "y"}},
            {.name = "via.vec3", .members = {"x", "y", "z"}},
            {.name = "via.vec4", .members = {"x", "y", "z", "w"}},
            {.name = "via.Quaternion", .members = {"x", "y", "z", "w"}},
            {.name = "via.Float2", .members = {"x", "y"}},
            {.name = "via.Float3", .members = {"x", "y", "z"}},
            {.name = "via.Float4", .members = {"x", "y", "z", "w"}},
            {.name = "via.Color", .members = {"rgba"}},
            {.name = "via.Range", .members = {"s", "r"}},
            {.name = "via.RangeI", .members = {"s", "r"}},
            {.name = "app.MOTION_SEQUENCE_FILTER_ID", .members = {"PageNo", "FilterNo"}},
            {.name = "System.Nullable`1<System.Int32>", .members = {"_HasValue", "_Value"}, .nullable = true},
            {.name = "System.Nullable`1<System.UInt32>", .members = {"_HasValue", "_Value"}, .nullable = true},
        };
        std::vector<SupportedValueLayout> result;
        result.reserve(std::size(descriptions));
        for (const auto& description : descriptions) {
            const auto* valueType = context.GetType(description.name);
            if (!valueType)
                continue;
            SupportedValueLayout layout{.type = valueType, .size = context.GetValueTypeSize(valueType), .nullable = description.nullable};
            for (const auto* name : description.members) {
                if (!name)
                    break;
                layout.members[layout.memberCount++] = context.GetFieldLayout(valueType, name);
            }
            result.push_back(layout);
        }
        return result;
    }();
    for (const auto& layout : layouts) {
        if (layout.type != type)
            continue;
        if (!layout.size || layout.size > sizeof(RE::SystemGuid))
            throw std::runtime_error("supported value exceeds the 16-byte layout contract");
        for (uint32_t index = 0; index < layout.memberCount; ++index) {
            const auto* field = layout.members[index];
            if (!field || !field->valueSize || GetScalarSize(context, field->type) != field->valueSize || uint64_t(field->valueOffset) + field->valueSize > layout.size)
                throw std::runtime_error("supported value member layout mismatch");
        }
        return &layout;
    }
    return nullptr;
}

// 新增整值/集合入口使用此查询；标量转换、算术及业务 key 仍保留 GetScalarSize 的限制。
uint32_t GetSupportedValueSize(const ci::ProviderContext& context, const ci::Type* type) {
    if (const auto* layout = FindSupportedValueLayout(context, type))
        return layout->size;
    return GetScalarSize(context, type);
}

// 将 JSON 整数、十进制字符串或 [数值] 显示文本精确转换为 T 后写入暂存地址。
// 全程保留 Int64/UInt64 精度并检查目标范围，不经 double 截断，也不接受部分解析。
template <class T>
static void WriteInteger(void* address, const Json& value) {
    T result{};
    if (value.is_string()) {
        std::string_view text = value.get_ref<const std::string&>();
        if (text.starts_with('[')) {
            const auto end = text.find(']');
            if (end == text.npos)
                throw std::runtime_error("invalid enum display value");
            text = text.substr(1, end - 1);
        }
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            throw std::runtime_error("integer text out of range");
    } else if (value.is_number_unsigned()) {
        const auto number = value.get<uint64_t>();
        if (number > uint64_t(std::numeric_limits<T>::max()))
            throw std::runtime_error("unsigned integer out of range");
        result = T(number);
    } else if (value.is_number_integer()) {
        const auto number = value.get<int64_t>();
        if constexpr (std::is_unsigned_v<T>) {
            if (number < 0 || uint64_t(number) > std::numeric_limits<T>::max())
                throw std::runtime_error("integer out of unsigned range");
        } else if (number < std::numeric_limits<T>::min() || number > std::numeric_limits<T>::max()) {
            throw std::runtime_error("signed integer out of range");
        }
        result = T(number);
    } else {
        throw std::runtime_error("integer requires an integer or decimal string");
    }
    memcpy(address, &result, sizeof(result));
}

// 释放构造器拥有的外部 USER 请求引用；普通新建托管对象不在这里逐一释放。
ObjectBuilder::~ObjectBuilder() {
    for (auto* object : ownedUserDataRequests)
        context.Release(object);
}

// 按目标字段类型把 JSON 写成引用或值 payload；转换失败抛异常，不默默截断。
// 依次处理引用、Guid、内嵌字段字典/vec3 bytes、Boolean、浮点和有界整数。
// 已发布引用必须传真实 owner 并走 AssignReference；owner=null 仅用于未发布数组或准备缓冲。
void ObjectBuilder::WriteValue(void* owner, void* destination, const ci::Type* type, uint32_t valueSize, const Json& value, std::optional<int32_t> elementIndex) {
    if (!type || !destination)
        throw std::runtime_error("value destination/type missing");
    static const auto* int32Type = context.GetType("System.Int32");
    static const auto* guidType = context.GetType("System.Guid");
    static const auto* vec3Type = context.GetType("via.vec3");
    static const auto* booleanType = context.GetType("System.Boolean");
    static const auto* singleType = context.GetType("System.Single");
    static const auto* doubleType = context.GetType("System.Double");
    if (!valueSize) {
        // 引用结果先构造完成：真实字段走写屏障，准备区/新数组只填尚未发布的槽。
        auto* object = CreateObject(type, value, elementIndex);
        if (owner) {
            if (!context.AssignReference(owner, static_cast<void**>(destination), object))
                throw std::runtime_error("reference assignment failed");
        } else {
            *static_cast<void**>(destination) = object;
        }
        return;
    }
    if (value == "$index" && type == int32Type) {
        if (!elementIndex)
            throw std::runtime_error("$index requires a collection element context");
        if (valueSize != sizeof(int32_t))
            throw std::runtime_error("$index requires System.Int32 layout");
        memcpy(destination, &*elementIndex, sizeof(int32_t));
        return;
    }
    if (type == guidType) {
        RE::SystemGuid guid;
        if (valueSize != sizeof(guid) || !value.is_string() || !guid.from_str(value.get_ref<const std::string&>()))
            throw std::runtime_error("invalid GUID value/layout");
        memcpy(destination, &guid, sizeof(guid));
        return;
    }
    if (value.is_object()) {
        if (value.contains("bytes")) {
            // vec3 有三个 float 分量；接受已核实的 12 字节 payload 和 16 字节对齐字段。
            constexpr size_t VEC3_PAYLOAD_SIZE = 3 * sizeof(float);
            constexpr size_t VEC3_ALIGNED_SIZE = 16;
            if (value.size() != 1 || type != vec3Type || (valueSize != VEC3_PAYLOAD_SIZE && valueSize != VEC3_ALIGNED_SIZE))
                throw std::runtime_error("raw bytes only support the verified vec3 payload");
            const auto text = value.at("bytes").get<std::string>();
            if (text.size() != VEC3_PAYLOAD_SIZE * 2) // 每字节对应两个十六进制字符。
                throw std::runtime_error("vec3 needs exactly 12 bytes");
            uint8_t bytes[VEC3_ALIGNED_SIZE]{};
            for (size_t i = 0; i < VEC3_PAYLOAD_SIZE; ++i) {
                unsigned int byte{};
                const auto parsed = std::from_chars(text.data() + i * 2, text.data() + i * 2 + 2, byte, 16);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + i * 2 + 2)
                    throw std::runtime_error("invalid vector hex");
                bytes[i] = uint8_t(byte);
            }
            memcpy(destination, bytes, valueSize);
            return;
        }
        if (value.size() != 1 || context.GetType(value.begin().key().c_str()) != type || !value.begin().value().is_object())
            throw std::runtime_error("inline value requires its exact type key and field dictionary");
        // 内嵌结构体只移动 payload 地址；其引用字段仍属于外层真实 managed owner。
        for (const auto& [name, child] : value.begin().value().items()) {
            const auto* field = context.GetFieldLayout(type, name.c_str());
            if (!field || uint64_t(field->valueOffset) + (field->valueSize ? field->valueSize : sizeof(void*)) > valueSize)
                throw std::runtime_error("inline field exceeds value layout: " + name);
            WriteValue(owner, static_cast<uint8_t*>(destination) + field->valueOffset, field->type, field->valueSize, child, elementIndex);
        }
        return;
    }
    if (type == booleanType) {
        if (valueSize != sizeof(bool) || !value.is_boolean())
            throw std::runtime_error("Boolean requires true/false and one-byte layout");
        const bool boolean = value.get<bool>();
        memcpy(destination, &boolean, sizeof(boolean));
        return;
    }
    if (type == singleType || type == doubleType) {
        if (GetScalarSize(context, type) != valueSize)
            throw std::runtime_error("floating field requires a number and matching layout");
        double number{};
        if (value.is_number()) {
            number = value.get<double>();
        } else if (value.is_string()) {
            const auto& text = value.get_ref<const std::string&>();
            const auto end = text.find(']');
            if (!text.starts_with('[') || end == text.npos)
                throw std::runtime_error("floating field requires a number or bracketed display value");
            const auto parsed = std::from_chars(text.data() + 1, text.data() + end, number);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + end)
                throw std::runtime_error("invalid floating display value");
        } else {
            throw std::runtime_error("floating field requires a number");
        }
        if (!std::isfinite(number) || (valueSize == sizeof(float) && std::abs(number) > std::numeric_limits<float>::max()))
            throw std::runtime_error("floating value out of range");
        if (valueSize == sizeof(float)) {
            const float scalar = float(number);
            memcpy(destination, &scalar, sizeof(scalar));
        } else {
            memcpy(destination, &number, sizeof(number));
        }
        return;
    }
    if (GetScalarSize(context, type) != valueSize)
        throw std::runtime_error("scalar layout size mismatch");
    const bool unsignedValue = IsUnsignedInteger(context, type);
    // 这里按 payload 的字节大小选择 C++ 整数类型，区别于 ElementType 的编码值。
    switch (valueSize) {
    case sizeof(int8_t):
        unsignedValue ? WriteInteger<uint8_t>(destination, value) : WriteInteger<int8_t>(destination, value);
        break;
    case sizeof(int16_t):
        unsignedValue ? WriteInteger<uint16_t>(destination, value) : WriteInteger<int16_t>(destination, value);
        break;
    case sizeof(int32_t):
        unsignedValue ? WriteInteger<uint32_t>(destination, value) : WriteInteger<int32_t>(destination, value);
        break;
    case sizeof(int64_t):
        unsignedValue ? WriteInteger<uint64_t>(destination, value) : WriteInteger<int64_t>(destination, value);
        break;
    default:
        throw std::runtime_error("unsupported integer width");
    }
}

// 按期望引用类型构造字符串、资源 holder、数组/List 或带类型键的托管对象。
// 先完整填充新集合/对象，再交调用方持有或发布；不额外调用通用构造函数或隐式 retain。
// $uri 只发起外部 USER 请求并由本构造器持有，不保证内容已解码；Prefab 路径引用另按原生布局处理。
void* ObjectBuilder::CreateObject(const ci::Type* expectedType, const Json& value, std::optional<int32_t> elementIndex) {
    if (!expectedType)
        throw std::runtime_error("expected reference type missing");
    static const auto* stringType = context.GetType("System.String");
    static const auto* resourceHolderType = context.GetType("via.ResourceHolder");
    static const auto* userDataType = context.GetType("via.UserData");
    static const auto* prefabType = context.GetType("via.Prefab");
    if (value.is_null())
        return nullptr;
    if (expectedType->objectType == std::to_underlying(via::clr::VMObjType::ValType))
        throw std::runtime_error("value type is not an object reference");
    if (expectedType == stringType) {
        const auto text = value.get<std::string>();
        auto* result = context.CreateString(vmContext, text.c_str());
        if (!result)
            throw std::runtime_error("string allocation failed");
        return result;
    }
    if (context.IsDerivedFrom(expectedType, resourceHolderType)) {
        if (!context.CreateResourceHolder || !value.is_string())
            throw std::runtime_error("resource holder requires a path string");
        const auto path = value.get<std::string>();
        if (path.empty() || path.contains('\0'))
            throw std::runtime_error("invalid resource path");
        auto* holder = context.CreateResourceHolder(vmContext, expectedType, path.c_str());
        if (!holder)
            throw std::runtime_error("resource holder creation failed: " + path);
        return holder;
    }
    if (value.is_array()) {
        if (value.size() > INT32_MAX)
            throw std::runtime_error("collection count exceeds System.Int32 range");
        const auto layout = GetCollectionLayout(context, expectedType);
        const auto* elementType = layout.elementType;
        const auto valueSize = layout.valueSize;
        const auto stride = layout.stride;
        auto* array = static_cast<RE::SystemArray<uint8_t>*>(context.CreateArray(vmContext, elementType, value.size(), false));
        if (!array || array == vmContext || array->contained_type != elementType || array->count != value.size())
            throw std::runtime_error("array allocation/type contract failed");
        // 先填满新数组再发布给 List/调用者，使首次 retain 时可扫描到完整引用元素。
        for (size_t i = 0; i < value.size(); ++i)
            WriteValue(nullptr, array->elements + stride * i, elementType, valueSize, value[i], int32_t(i));
        if (!layout.itemsField)
            return array;
        auto* list = context.CreateInstance(vmContext, expectedType, false);
        if (!list)
            throw std::runtime_error("List allocation/layout failed");
        if (!context.AssignReference(list, context.GetField<void*>(list, layout.itemsField), array))
            throw std::runtime_error("List items assignment failed");
        *context.GetField<int32_t>(list, layout.sizeField) = int32_t(value.size());
        return list;
    }
    if (!value.is_object() || value.size() != 1 || !value.begin().value().is_object())
        throw std::runtime_error("object requires one type key and a field dictionary");
    const auto& typeName = value.begin().key();
    const auto* type = context.GetType(typeName.c_str());
    if (!context.IsDerivedFrom(type, expectedType) || type->objectType == std::to_underlying(via::clr::VMObjType::ValType) || type->objectType == std::to_underlying(via::clr::VMObjType::Array))
        throw std::runtime_error("reference type mismatch: " + typeName);
    const auto& fields = value.begin().value();
    if (fields.contains("$uri")) {
        if (fields.size() != 1 || !fields.at("$uri").is_string() || !context.IsDerivedFrom(type, userDataType) || !context.CreateUserData)
            throw std::runtime_error("$uri requires a USER type and must not mix with fields");
        const auto path = fields.at("$uri").get<std::string>();
        if (path.empty() || path.contains('\0'))
            throw std::runtime_error("invalid external USER path");
        auto* object = context.CreateUserData(type, path.c_str());
        if (!object)
            throw std::runtime_error("external USER request failed: " + path);
        // 工厂已交出一份 owned 引用，构造器负责归还；调用方若要跨批保存须另行持有。
        ownedUserDataRequests.push_back(object);
        return object;
    }
    auto* object = context.CreateInstance(vmContext, type, false);
    if (!object)
        throw std::runtime_error("object allocation failed: " + typeName);
    const auto* createdType = context.GetInstanceType(object);
    if (createdType != type)
        throw std::runtime_error("instance allocator returned a different type: " + typeName);
    if (type == prefabType) {
        static const auto set_Standby = reinterpret_cast<Fnset_Standby>(context.GetMethod(prefabType, "set_Standby(System.Boolean)"));
        if (!set_Standby || fields.size() != 2 || !fields.contains("Path") || !fields.contains("Standby") || !fields.at("Standby").is_boolean())
            throw std::runtime_error("Prefab requires Standby and Path");
        set_Standby(vmContext, object, false);
        auto* path = CreateObject(stringType, fields.at("Path"));
        context.Retain(path); // 原生 +0x18 不是 GC 扫描的托管字段。
        static_cast<RE::REPrefab*>(object)->path = static_cast<RE::SystemString*>(path);
        if (fields.at("Standby").get<bool>())
            set_Standby(vmContext, object, true);
        return object;
    }
    for (const auto& [name, child] : fields.items()) {
        const auto* field = context.GetFieldLayout(type, name.c_str());
        if (!field || name.starts_with('$'))
            throw std::runtime_error("instance field missing: " + typeName + "." + name);
        WriteValue(object, context.GetField(object, field), field->type, field->valueSize, child, elementIndex);
    }
    return object;
}

// 在元数据阶段递归检查 JSON 与目标字段/集合类型是否兼容，不创建托管对象或请求资源。
// 标量复用真实转换规则写入局部缓冲；引用只校验派生关系、字段、资源路径和所需 CI 能力。
void ValidateJsonValue(const ci::ProviderContext& context, const ci::Type* type, uint32_t valueSize, const Json& value, std::optional<int32_t> elementIndex) {
    if (!type)
        throw std::runtime_error("JSON target type missing");
    static const auto* stringType = context.GetType("System.String");
    static const auto* resourceHolderType = context.GetType("via.ResourceHolder");
    static const auto* userDataType = context.GetType("via.UserData");
    static const auto* prefabType = context.GetType("via.Prefab");
    if (valueSize) {
        if (value.is_object() && !value.contains("bytes")) {
            if (value.size() != 1 || context.GetType(value.begin().key().c_str()) != type || !value.begin().value().is_object())
                throw std::runtime_error("inline value requires its exact type key");
            for (const auto& [name, child] : value.begin().value().items()) {
                const auto* field = context.GetFieldLayout(type, name.c_str());
                if (!field || uint64_t(field->valueOffset) + (field->valueSize ? field->valueSize : sizeof(void*)) > valueSize)
                    throw std::runtime_error("inline field exceeds value layout: " + name);
                ValidateJsonValue(context, field->type, field->valueSize, child, elementIndex);
            }
        } else {
            // 绑定期只验证标量，暂存空间按 v1 最大标量 Guid 的大小分配，不创建托管对象。
            alignas(std::max_align_t) uint8_t bytes[sizeof(RE::SystemGuid)]{};
            if (valueSize > sizeof(bytes))
                throw std::runtime_error("unsupported scalar value layout");
            ObjectBuilder(context, nullptr).WriteValue(nullptr, bytes, type, valueSize, value, elementIndex);
        }
        return;
    }
    if (type->objectType == std::to_underlying(via::clr::VMObjType::ValType))
        throw std::runtime_error("value type has a reference layout");
    if (value.is_null())
        return;
    if (type == stringType) {
        if (!value.is_string())
            throw std::runtime_error("System.String requires a JSON string");
        return;
    }
    if (context.IsDerivedFrom(type, resourceHolderType)) {
        if (!context.CreateResourceHolder || !value.is_string() || value.get_ref<const std::string&>().empty() || value.get_ref<const std::string&>().contains('\0'))
            throw std::runtime_error("ResourceHolder requires a valid path and CI factory");
        return;
    }
    if (value.is_array()) {
        if (value.size() > INT32_MAX)
            throw std::runtime_error("collection count exceeds System.Int32 range");
        const auto layout = GetCollectionLayout(context, type);
        const auto* elementType = layout.elementType;
        const auto elementSize = layout.valueSize;
        // 绑定期只验证存在元素上下文；构造时再传入每个元素的真实下标。
        for (const auto& item : value)
            ValidateJsonValue(context, elementType, elementSize, item, 0);
        return;
    }
    if (!value.is_object() || value.size() != 1 || !value.begin().value().is_object())
        throw std::runtime_error("reference requires one type key and field dictionary");
    const auto* actualType = context.GetType(value.begin().key().c_str());
    if (!context.IsDerivedFrom(actualType, type) || actualType->objectType == std::to_underlying(via::clr::VMObjType::Array) || actualType->objectType == std::to_underlying(via::clr::VMObjType::ValType))
        throw std::runtime_error("reference type mismatch: " + value.begin().key());
    const auto& fields = value.begin().value();
    if (fields.contains("$uri")) {
        if (fields.size() != 1 || !fields.at("$uri").is_string() || fields.at("$uri").get_ref<const std::string&>().empty() || fields.at("$uri").get_ref<const std::string&>().contains('\0') || !context.IsDerivedFrom(actualType, userDataType) || !context.CreateUserData)
            throw std::runtime_error("invalid external USER declaration");
        return;
    }
    if (actualType == prefabType) {
        static const auto set_Standby = context.GetMethod(prefabType, "set_Standby(System.Boolean)");
        if (fields.size() != 2 || !fields.contains("Standby") || !fields.at("Standby").is_boolean() || !fields.contains("Path") || !fields.at("Path").is_string() || !set_Standby)
            throw std::runtime_error("Prefab requires Standby, Path and native setter");
        return;
    }
    for (const auto& [name, child] : fields.items()) {
        const auto* field = context.GetFieldLayout(actualType, name.c_str());
        if (!field || name.starts_with('$'))
            throw std::runtime_error("instance field missing: " + name);
        ValidateJsonValue(context, field->type, field->valueSize, child, elementIndex);
    }
}
} // namespace UserDataExtension
