#include "UserDataFieldPath.h"
#include "UserDataCollection.h"
#include "UserDataConfiguration.h"
#include "UserDataObjectBuilder.h"
#include <GameTypes.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace UserDataExtension {
using Json = nlohmann::json;

// 判断一段字段名是否非空且不含路径分隔符、选择器符号或空白。
static bool IsFieldName(std::string_view name) {
    return !name.empty() && name.find_first_of(".[]= \t\r\n\"") == name.npos;
}

// 把字段路径解析为 Field、Index、Match 步骤，只做语法检查，不访问游戏对象。
// 扫描选择器时跟踪引号和转义，避免把字符串内的 ] 或 = 当作边界；匹配值须为非空 JSON 标量。
// 匹配 key 仅允许多级字段，不允许在 key 中再次嵌套集合选择器。
std::vector<FieldPathStep> ParseFieldPath(std::string_view path) {
    std::vector<FieldPathStep> steps;
    size_t position = 0;
    bool needsField = true;
    while (position < path.size()) {
        if (needsField) {
            const auto end = path.find_first_of(".[", position);
            const auto field = path.substr(position, end == path.npos ? path.size() - position : end - position);
            if (!IsFieldName(field))
                throw std::runtime_error("invalid field name in path: " + std::string(path));
            steps.push_back({.kind = FieldPathStepKind::Field, .fieldName = std::string(field)});
            position += field.size();
            needsField = false;
            continue;
        }
        if (path[position] == '.') {
            ++position;
            needsField = true;
            continue;
        }
        if (path[position] != '[')
            throw std::runtime_error("invalid field path separator: " + std::string(path));
        const auto begin = ++position;
        // 引号内的分隔符只是 JSON 字符串内容，必须读到真正的选择器结束符。
        bool quoted = false;
        bool escaped = false;
        size_t equal = path.npos;
        for (; position < path.size(); ++position) {
            const char character = path[position];
            if (quoted) {
                if (escaped)
                    escaped = false;
                else if (character == '\\')
                    escaped = true;
                else if (character == '"')
                    quoted = false;
            } else if (character == '"') {
                quoted = true;
            } else if (character == ']') {
                break;
            } else if (character == '=' && equal == path.npos) {
                equal = position;
            } else if (character == '[') {
                throw std::runtime_error("nested selector is not supported: " + std::string(path));
            }
        }
        if (position == path.size() || quoted)
            throw std::runtime_error("unterminated field selector: " + std::string(path));
        if (equal == path.npos) {
            const auto text = path.substr(begin, position - begin);
            size_t index{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), index);
            if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                throw std::runtime_error("invalid collection index: " + std::string(text));
            steps.push_back({.kind = FieldPathStepKind::Index, .index = index});
        } else {
            const auto key = path.substr(begin, equal - begin);
            auto keySteps = ParseFieldPath(key);
            if (std::ranges::any_of(keySteps, [](const auto& step) {
                    return step.kind != FieldPathStepKind::Field;
                }))
                throw std::runtime_error("selector key must be a field path");
            auto value = Json::parse(path.substr(equal + 1, position - equal - 1));
            if (!value.is_primitive() || value.is_null())
                throw std::runtime_error("selector value must be a non-null JSON scalar");
            steps.push_back({.kind = FieldPathStepKind::Match, .key = std::string(key), .keySteps = std::move(keySteps), .value = std::move(value)});
        }
        ++position;
    }
    if (steps.empty() || needsField)
        throw std::runtime_error("empty or incomplete field path: " + std::string(path));
    return steps;
}

// 沿声明类型推导路径末端类型，并验证集合选择器、匹配 key 及预期值的类型。
// 引用集合中的实际元素可为派生类：只有这种缺字段情况允许返回 nullptr，延后到现场检查。
// 值类型和已明确的类型错误仍立即拒绝，不把类型未知当作字段一定存在。
static ci::TypeDefinitionHandle ValidatePathType(const ci::SDKAPI& context, ci::TypeDefinitionHandle type, std::span<const FieldPathStep> steps, bool runtimeElementType) {
    static const auto* userDataType = context.tdb->find_type("via.UserData");
    static const auto* stringType = context.tdb->find_type("System.String");
    for (const auto& step : steps) {
        if (step.kind == FieldPathStepKind::Field) {
            const auto field = ci::API::TypeDefinition{&context, type}.find_field(step.fieldName.c_str());
            // 引用集合可以声明基类、保存派生行。缺成员时只延迟布局检查，运行期仍须精确定位。
            if ((!field || field.is_static()) && runtimeElementType)
                return nullptr;
            if (!field || field.is_static())
                throw std::runtime_error("instance field missing: " + step.fieldName);
            type = field.get_type().handle;
            if (field.get_type().get_valuetype_size())
                runtimeElementType = false;
        } else {
            const auto layout = GetCollectionLayout(context, type);
            const auto* elementType = layout.elementType;
            type = elementType;
            runtimeElementType = !layout.valueSize;
            if (step.kind == FieldPathStepKind::Match) {
                if (step.key == "$uri") {
                    if (!context.type_definition->is_derived_from(elementType, userDataType) || !step.value.is_string())
                        throw std::runtime_error("$uri selector requires USER elements and a string");
                    const auto uri = NormalizeUserDataUri(step.value.get<std::string>());
                    if (uri.empty() || uri.contains('\0'))
                        throw std::runtime_error("$uri selector requires a nonempty URI without NUL");
                    continue;
                }
                if (layout.valueSize)
                    throw std::runtime_error("field matcher requires reference elements");
                const auto identity = ValidatePathType(context, elementType, step.keySteps, true);
                if (!identity)
                    continue;
                const auto identitySize = context.type_definition->get_valuetype_size(identity);
                if ((!identitySize && identity != stringType) || identitySize > sizeof(RE::SystemGuid))
                    throw std::runtime_error("selector identity must be scalar, Guid or string");
                if (identitySize && GetScalarSize(context, identity) != identitySize)
                    throw std::runtime_error("selector identity layout is not a supported scalar");
                ValidateJsonValue(context, identity, identitySize, step.value);
            }
        }
    }
    return type;
}

// 从声明根类型校验完整路径，返回已知末端类型；nullptr 表示依赖运行时派生行。
ci::TypeDefinitionHandle ValidateFieldPath(const ci::SDKAPI& context, ci::TypeDefinitionHandle rootType, std::span<const FieldPathStep> steps) {
    return ValidatePathType(context, rootType, steps, false);
}

// 阻止解引用空父对象或本轮尚未解码的外部 USER 请求。
// 外部资源应作为独立 target，在它自己的解码完成回调中处理。
static void CheckDecodedObject(void* object, std::span<void* const> externalObjects) {
    if (!object)
        throw std::runtime_error("null parent in USER field path");
    if (std::ranges::find(externalObjects, object) != externalObjects.end())
        throw std::runtime_error("field path crosses an external USER request; register that USER as a separate target");
}

// 按真实字段类型转换选择器值，再与当前字段比较：字符串比内容，整数/Guid 比 payload。
// 浮点按数值精确相等匹配并拒绝非有限当前值，因而 +0 与 -0 可命中同一行。
static bool MatchesSelector(const ci::SDKAPI& context, void* vmContext, const FieldLocation& location, const Json& expected) {
    static const auto* stringType = context.tdb->find_type("System.String");
    static const auto* singleType = context.tdb->find_type("System.Single");
    static const auto* doubleType = context.tdb->find_type("System.Double");
    if (location.type == stringType) {
        const auto* string = *static_cast<RE::SystemString**>(location.address);
        return expected.is_string() && string && string->to_str() == expected.get<std::string>();
    }
    if (!location.valueSize || location.valueSize > sizeof(RE::SystemGuid))
        throw std::runtime_error("unsupported selector identity type");
    uint8_t bytes[sizeof(RE::SystemGuid)]{};
    ObjectBuilder builder(context, vmContext);
    builder.WriteValue(nullptr, bytes, location.type, location.valueSize, expected);
    if (location.type == singleType) {
        float value{}, candidate{};
        std::memcpy(&value, location.address, sizeof(value));
        std::memcpy(&candidate, bytes, sizeof(candidate));
        return std::isfinite(value) && value == candidate;
    }
    if (location.type == doubleType) {
        double value{}, candidate{};
        std::memcpy(&value, location.address, sizeof(value));
        std::memcpy(&candidate, bytes, sizeof(candidate));
        return std::isfinite(value) && value == candidate;
    }
    return std::memcmp(location.address, bytes, location.valueSize) == 0;
}

// 在已解码对象图上定位实际字段槽，返回借用地址、类型、大小及引用写屏障所需的 owner。
// 逐步处理字段、数组/List 下标和唯一匹配选择器；每次跨引用都检查外部 USER 解码边界。
// 内嵌值使用 payload 相对偏移，引用对象使用实际实例布局；记录祖先引用槽供同批冲突检查。
FieldLocation ResolveFieldPath(const ci::SDKAPI& context, void* vmContext, void* root, std::span<const FieldPathStep> steps, std::span<void* const> externalObjects) {
    CheckDecodedObject(root, externalObjects);
    auto* object = root;
    const auto* type = context.managed_object->get_type_definition(root);
    bool inlineValue = false;
    FieldLocation result{.owner = root};
    // address 跟随路径前进；owner 只在跨托管引用/进入数组元素区时变化。
    for (size_t i = 0; i < steps.size(); ++i) {
        const auto& step = steps[i];
        if (step.kind == FieldPathStepKind::Field) {
            const auto field = ci::API::TypeDefinition{&context, type}.find_field(step.fieldName.c_str());
            if (!field || field.is_static())
                throw std::runtime_error("instance field missing: " + step.fieldName);
            result.address = field.get_data_raw(object, inlineValue);
            result.type = field.get_type().handle;
            result.valueSize = field.get_type().get_valuetype_size();
            if (!result.address)
                throw std::runtime_error("instance field address is missing: " + step.fieldName);
        } else {
            if (result.valueSize)
                throw std::runtime_error("collection selector applied to a value field");
            auto* collection = *static_cast<void**>(result.address);
            CheckDecodedObject(collection, externalObjects);
            result.ancestorReferenceSlots.push_back(result.address);
            const auto* collectionType = context.managed_object->get_type_definition(collection);
            const auto layout = GetCollectionLayout(context, collectionType);
            const auto view = ReadCollection(context, collectionType, collection);
            if (layout.itemsField)
                result.ancestorReferenceSlots.push_back(layout.itemsField.get_data_raw(collection));
            auto* array = view.items;
            const auto* elementType = view.elementType;
            const auto count = view.count;
            const auto valueSize = view.valueSize;
            const auto stride = view.stride;
            size_t index = step.index;
            if (step.kind == FieldPathStepKind::Match) {
                if (valueSize)
                    throw std::runtime_error("field matcher requires reference elements");
                using Fnget_URI = RE::SystemString* (*)(void* vmContext, void* userData);
                static const auto* userDataType = context.tdb->find_type("via.UserData");
                static const auto get_URI = ci::API::TypeDefinition{&context, userDataType}.find_method("get_URI()").get_function<Fnget_URI>();
                std::string expectedUri;
                if (step.key == "$uri") {
                    if (!get_URI || !context.type_definition->is_derived_from(elementType, userDataType) || !step.value.is_string())
                        throw std::runtime_error("$uri selector requires USER elements and get_URI");
                    expectedUri = NormalizeUserDataUri(step.value.get<std::string>());
                }
                size_t matches = 0;
                for (size_t j = 0; j < count; ++j) {
                    auto* row = *reinterpret_cast<void**>(array->elements + j * stride);
                    if (!row)
                        continue;
                    bool matched = false;
                    if (step.key == "$uri") {
                        const auto* uri = get_URI(vmContext, row);
                        matched = uri && NormalizeUserDataUri(uri->to_str()) == expectedUri;
                    } else {
                        const auto identity = ResolveFieldPath(context, vmContext, row, step.keySteps, externalObjects);
                        matched = MatchesSelector(context, vmContext, identity, step.value);
                    }
                    if (matched) {
                        index = j;
                        ++matches;
                    }
                }
                if (matches != 1)
                    throw std::runtime_error("field selector must match exactly one row; matches=" + std::to_string(matches));
            }
            if (index >= count)
                throw std::runtime_error("collection index is out of range");
            result.owner = array;
            result.address = array->elements + index * stride;
            result.type = elementType;
            result.valueSize = valueSize;
        }
        if (i + 1 == steps.size())
            return result;
        // 下一步是集合选择时保留字段槽，不把数组对象当成普通字段父对象。
        if (steps[i + 1].kind != FieldPathStepKind::Field)
            continue;
        inlineValue = result.valueSize != 0;
        object = inlineValue ? result.address : *static_cast<void**>(result.address);
        if (!inlineValue) {
            result.ancestorReferenceSlots.push_back(result.address);
            CheckDecodedObject(object, externalObjects);
            result.owner = object;
            type = context.managed_object->get_type_definition(object);
        } else {
            type = result.type;
        }
    }
    throw std::runtime_error("invalid USER field path");
}
} // namespace UserDataExtension
