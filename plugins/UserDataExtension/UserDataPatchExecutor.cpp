#include "UserDataPatchExecutor.h"
#include "UserDataCollection.h"
#include "UserDataFieldPath.h"
#include "UserDataObjectBuilder.h"
#include <GameTypes.h>
#include <LogUtils.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>
#include <variant>

namespace UserDataExtension {
using Fnget_URI = RE::SystemString* (*)(void*, void*);

// 把字段值编码为去重身份：类型 ID 前缀加标量 payload，字符串使用 UTF-8 内容。
// 浮点拒绝 NaN/无穷并把正负零归一，避免同一数值产生不同业务身份。
static std::string ReadScalarIdentity(const ci::ProviderContext& context, const ci::Type* type, uint32_t valueSize, const void* address) {
    static const auto* stringType = context.GetType("System.String");
    static const auto* singleType = context.GetType("System.Single");
    static const auto* doubleType = context.GetType("System.Double");
    const uint32_t typeIndex = uint32_t(type->index);
    std::string identity(reinterpret_cast<const char*>(&typeIndex), sizeof(typeIndex));
    if (!valueSize) {
        if (type != stringType)
            throw std::runtime_error("identity must be scalar, Guid or string");
        const auto* string = *static_cast<RE::SystemString* const*>(address);
        if (!string)
            throw std::runtime_error("identity string is null");
        return identity.append(string->to_str());
    }
    if (GetScalarSize(context, type) != valueSize)
        throw std::runtime_error("identity layout mismatch");
    if (type == singleType) {
        float number;
        memcpy(&number, address, sizeof(number));
        if (!std::isfinite(number))
            throw std::runtime_error("identity float is not finite");
        if (number == 0)
            number = 0; // +0 与 -0 是同一业务身份。
        return identity.append(reinterpret_cast<const char*>(&number), sizeof(number));
    }
    if (type == doubleType) {
        double number;
        memcpy(&number, address, sizeof(number));
        if (!std::isfinite(number))
            throw std::runtime_error("identity double is not finite");
        if (number == 0)
            number = 0;
        return identity.append(reinterpret_cast<const char*>(&number), sizeof(number));
    }
    return identity.append(static_cast<const char*>(address), valueSize);
}

// 简单纯值结构按有效成员形成身份；不比较 padding，不引入任意对象图或旋转等价判断。
static std::string ReadValueIdentity(const ci::ProviderContext& context, const ci::Type* type, uint32_t valueSize, const void* address) {
    const auto* layout = FindSupportedValueLayout(context, type);
    if (!layout)
        return ReadScalarIdentity(context, type, valueSize, address);
    if (layout->size != valueSize)
        throw std::runtime_error("value identity layout mismatch");
    const uint32_t typeIndex = uint32_t(type->index);
    std::string identity(reinterpret_cast<const char*>(&typeIndex), sizeof(typeIndex));
    for (uint32_t index = 0; index < layout->memberCount; ++index) {
        const auto* member = layout->members[index];
        const auto* memberAddress = static_cast<const uint8_t*>(address) + member->valueOffset;
        identity += ReadScalarIdentity(context, member->type, member->valueSize, memberAddress);
        if (layout->nullable && index == 0 && !*reinterpret_cast<const bool*>(memberAddress))
            break; // Nullable 空值的 _Value 未生效，不参与身份。
    }
    return identity;
}

// 读取已有集合元素的业务身份：标量/简单值按值，对象沿 key 路径，USER 可用规范化 $uri。
// 读取对象字段时沿用外部 USER 边界检查；这里只借用现有元素，不构造候选行。
static std::string ReadElementIdentity(const ci::ProviderContext& context, void* vmContext, const CollectionView& collection, const void* address, const std::string& key, std::span<const FieldPathStep> keySteps, std::span<void* const> externalObjects) {
    static const auto* stringType = context.GetType("System.String");
    static const auto* userDataType = context.GetType("via.UserData");
    if (collection.valueSize || collection.elementType == stringType) {
        if (!key.empty())
            throw std::runtime_error("scalar collection identity does not accept key");
        return ReadValueIdentity(context, collection.elementType, collection.valueSize, address);
    }
    auto* object = *static_cast<void* const*>(address);
    if (!object)
        throw std::runtime_error("object identity is null");
    if (key == "$uri") {
        if (!context.IsDerivedFrom(context.GetInstanceType(object), userDataType))
            throw std::runtime_error("$uri identity requires a USER object");
        static const auto get_URI = reinterpret_cast<Fnget_URI>(context.GetMethod(userDataType, "get_URI()"));
        if (!get_URI)
            throw std::runtime_error("via.UserData.get_URI entry missing");
        const auto* uri = get_URI(vmContext, object);
        if (!uri || !uri->size)
            throw std::runtime_error("USER identity URI is empty");
        return NormalizeUserDataUri(uri->to_str());
    }
    if (key.empty())
        throw std::runtime_error("object collection requires a business key");
    const auto identity = ResolveFieldPath(context, vmContext, object, keySteps, externalObjects);
    return ReadScalarIdentity(context, identity.type, identity.valueSize, identity.address);
}

// 候选 JSON 中的字段视图；value=null 可表示省略的零初始化值字段，不拥有 JSON。
struct DeclaredFieldValue {
    const ci::Type* type{};
    uint32_t valueSize{};
    const Json* value{};
};

// 从声明中的标量生成与现有字段相同的身份编码；省略的值字段使用新对象的零初值。
// 字符串直接读取 JSON 内容，不为去重分配托管字符串；缺少非空字符串身份视为声明错误。
static std::string ReadDeclaredScalarIdentity(const ci::ProviderContext& context, ObjectBuilder& builder, const DeclaredFieldValue& field) {
    static const auto* stringType = context.GetType("System.String");
    static const auto* int32Type = context.GetType("System.Int32");
    if (!field.valueSize) {
        if (field.type != stringType || !field.value || !field.value->is_string())
            throw std::runtime_error("identity must be a non-null scalar, Guid or string");
        const uint32_t typeIndex = uint32_t(field.type->index);
        return std::string(reinterpret_cast<const char*>(&typeIndex), sizeof(typeIndex)) + field.value->get_ref<const std::string&>();
    }
    alignas(RE::SystemGuid) uint8_t scalar[sizeof(RE::SystemGuid)]{};
    if (GetScalarSize(context, field.type) != field.valueSize)
        throw std::runtime_error("identity layout mismatch");
    if (field.type == int32Type && field.value && *field.value == "$index")
        throw std::runtime_error("$index must not be used as a numeric business identity");
    if (field.value)
        builder.WriteValue(nullptr, scalar, field.type, field.valueSize, *field.value);
    // 省略的值字段沿用新对象的零初始化，不为读取身份分配托管对象。
    return ReadScalarIdentity(context, field.type, field.valueSize, scalar);
}

// 沿 JSON 类型键/字段字典推导候选行的身份字段，也支持声明数组中的下标或唯一匹配。
// 只读声明与 TDB，缺省内嵌值按零初始化继续推导，不为取 key 构造完整对象或请求 USER。
static DeclaredFieldValue ResolveDeclaredFieldPath(const ci::ProviderContext& context, ObjectBuilder& builder, DeclaredFieldValue field, std::span<const FieldPathStep> steps) {
    for (const auto& step : steps) {
        if (step.kind == FieldPathStepKind::Field) {
            const Json* fields = nullptr;
            if (field.value && field.value->is_object() && field.value->size() == 1) {
                field.type = context.GetType(field.value->begin().key().c_str());
                fields = &field.value->begin().value();
            } else if (field.value || !field.valueSize) {
                throw std::runtime_error("identity member path requires a typed field dictionary");
            }
            const auto* member = context.GetFieldLayout(field.type, step.fieldName.c_str());
            if (!member)
                throw std::runtime_error("identity field missing: " + step.fieldName);
            const auto found = fields ? fields->find(step.fieldName) : Json::const_iterator{};
            field = {.type = member->type, .valueSize = member->valueSize, .value = fields && found != fields->end() ? &*found : nullptr};
            continue;
        }
        if (!field.value || !field.value->is_array())
            throw std::runtime_error("identity selector requires a declared collection");
        const auto layout = GetCollectionLayout(context, field.type);
        size_t index = step.index;
        if (step.kind == FieldPathStepKind::Match) {
            size_t matches = 0;
            for (size_t i = 0; i < field.value->size(); ++i) {
                const auto& row = field.value->at(i);
                // 与运行时选择器一致：引用集合中的空槽不参与业务 key 匹配。
                if (!layout.valueSize && row.is_null())
                    continue;
                bool matched;
                if (step.key == "$uri") {
                    matched = NormalizeUserDataUri(row.begin().value().at("$uri").get<std::string>()) == NormalizeUserDataUri(step.value.get<std::string>());
                } else {
                    const auto key = ResolveDeclaredFieldPath(context, builder, {.type = layout.elementType, .valueSize = layout.valueSize, .value = &row}, step.keySteps);
                    matched = ReadDeclaredScalarIdentity(context, builder, key) == ReadDeclaredScalarIdentity(context, builder, {.type = key.type, .valueSize = key.valueSize, .value = &step.value});
                }
                if (matched) {
                    index = i;
                    ++matches;
                }
            }
            if (matches != 1)
                throw std::runtime_error("declared identity selector must match exactly one row");
        }
        if (index >= field.value->size())
            throw std::runtime_error("declared identity index is out of range");
        field = {.type = layout.elementType, .valueSize = layout.valueSize, .value = &field.value->at(index)};
    }
    return field;
}

// 在创建候选元素前提取其身份；$uri 直接规范化声明路径，对象 key 走声明字段路径。
// 与读取已有元素共用身份编码，使已存在 key 能直接跳过而不产生无用资源请求。
static std::string ReadDeclaredElementIdentity(const ci::ProviderContext& context, ObjectBuilder& builder, DeclaredFieldValue field, const Operation& operation) {
    static const auto* stringType = context.GetType("System.String");
    if (field.valueSize && FindSupportedValueLayout(context, field.type)) {
        alignas(16) uint8_t bytes[sizeof(RE::SystemGuid)]{};
        if (field.value)
            builder.WriteValue(nullptr, bytes, field.type, field.valueSize, *field.value);
        return ReadValueIdentity(context, field.type, field.valueSize, bytes);
    }
    if (!field.valueSize && field.type != stringType) {
        if (operation.key == "$uri")
            return NormalizeUserDataUri(field.value->begin().value().at("$uri").get<std::string>());
        field = ResolveDeclaredFieldPath(context, builder, field, operation.keySteps);
    }
    return ReadDeclaredScalarIdentity(context, builder, field);
}

// 执行 min/max 或有界整数四则运算，在发生 C++ 溢出/除零之前检查边界。
// 分别处理有符号/无符号范围，以及最小有符号值除以 -1 的特殊溢出。
template <class T>
static T CalculateInteger(T current, T operand, OperationKind operation) {
    constexpr auto min = std::numeric_limits<T>::min();
    constexpr auto max = std::numeric_limits<T>::max();
    if (operation == OperationKind::Minimum)
        return std::min(current, operand);
    if (operation == OperationKind::Maximum)
        return std::max(current, operand);
    if (operation == OperationKind::Add) {
        if constexpr (std::is_unsigned_v<T>) {
            if (current > max - operand)
                throw std::runtime_error("integer addition overflow");
        } else if ((operand > 0 && current > max - operand) || (operand < 0 && current < min - operand)) {
            throw std::runtime_error("integer addition overflow");
        }
        return T(current + operand);
    }
    if (operation == OperationKind::Subtract) {
        if constexpr (std::is_unsigned_v<T>) {
            if (current < operand)
                throw std::runtime_error("unsigned subtraction underflow");
        } else if ((operand > 0 && current < min + operand) || (operand < 0 && current > max + operand)) {
            throw std::runtime_error("integer subtraction overflow");
        }
        return T(current - operand);
    }
    if (operation == OperationKind::Multiply) {
        // 先按符号组合比较除法阈值，不能先相乘再检查，否则有符号溢出已经发生。
        if constexpr (std::is_unsigned_v<T>) {
            if (operand && current > max / operand)
                throw std::runtime_error("integer multiplication overflow");
        } else if ((current > 0 && ((operand > 0 && current > max / operand) || (operand < 0 && operand < min / current))) ||
                   (current < 0 && ((operand > 0 && current < min / operand) || (operand < 0 && current < max / operand)))) {
            throw std::runtime_error("integer multiplication overflow");
        }
        return T(current * operand);
    }
    if (operation == OperationKind::Divide) {
        if (!operand)
            throw std::runtime_error("division by zero");
        if constexpr (std::is_signed_v<T>)
            if (current == min && operand == -1)
                throw std::runtime_error("integer division overflow");
        return T(current / operand);
    }
    throw std::runtime_error("unknown numeric operation");
}

// 读取当前值与操作数，计算后仅写准备缓冲，并返回旧值到新值的日志文本。
// 浮点同时检查输入/结果有限性；除数精确等于零检查用于禁止除零，不是误差近似判断。
template <class T>
static std::string PrepareNumber(const void* currentAddress, const void* operandAddress, void* resultAddress, OperationKind operation) {
    T current, operand, result;
    memcpy(&current, currentAddress, sizeof(T));
    memcpy(&operand, operandAddress, sizeof(T));
    if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(current) || !std::isfinite(operand))
            throw std::runtime_error("floating input is not finite");
        if (operation == OperationKind::Add)
            result = current + operand;
        else if (operation == OperationKind::Subtract)
            result = current - operand;
        else if (operation == OperationKind::Multiply)
            result = current * operand;
        else if (operation == OperationKind::Divide) {
            if (operand == 0)
                throw std::runtime_error("division by zero");
            result = current / operand;
        } else if (operation == OperationKind::Minimum)
            result = std::min(current, operand);
        else if (operation == OperationKind::Maximum)
            result = std::max(current, operand);
        else
            throw std::runtime_error("unknown numeric operation");
        if (!std::isfinite(result))
            throw std::runtime_error("floating result is not finite");
    } else {
        result = CalculateInteger(current, operand, operation);
    }
    memcpy(resultAddress, &result, sizeof(T));
    return Json(current).dump() + " -> " + Json(result).dump();
}

// 限定可做算术的基础数值类型；枚举虽可赋值，但不因其整数底层布局自动开放算术。
static bool IsNumericType(const ci::ProviderContext& context, const ci::Type* type) {
    static const ci::Type* const numericTypes[]{context.GetType("System.Byte"), context.GetType("System.SByte"), context.GetType("System.Int16"), context.GetType("System.UInt16"), context.GetType("System.Char"), context.GetType("System.Int32"), context.GetType("System.UInt32"), context.GetType("System.Int64"), context.GetType("System.UInt64"), context.GetType("System.Single"), context.GetType("System.Double")};
    for (const auto* numericType : numericTypes)
        if (type == numericType)
            return true;
    return false;
}

// 验证数值字段布局，转换操作数，再按浮点类型或整数宽度/符号派发计算。
// 只写 result 暂存区，不写 location 指向的原字段，保证后续准备失败时不提前提交。
static std::string PrepareArithmetic(const ci::ProviderContext& context, ObjectBuilder& builder, const FieldLocation& location, const Operation& operation, void* result) {
    if (!location.valueSize || !IsNumericType(context, location.type) || GetScalarSize(context, location.type) != location.valueSize)
        throw std::runtime_error("arithmetic requires a plain numeric field");
    static const auto* singleType = context.GetType("System.Single");
    static const auto* doubleType = context.GetType("System.Double");
    alignas(std::max_align_t) uint8_t operand[sizeof(RE::SystemGuid)]{};
    builder.WriteValue(nullptr, operand, location.type, location.valueSize, operation.value);
    if (location.type == singleType)
        return PrepareNumber<float>(location.address, operand, result, operation.kind);
    if (location.type == doubleType)
        return PrepareNumber<double>(location.address, operand, result, operation.kind);
    const bool unsignedValue = IsUnsignedInteger(context, location.type);
    switch (location.valueSize) {
    case sizeof(uint8_t):
        return unsignedValue ? PrepareNumber<uint8_t>(location.address, operand, result, operation.kind) : PrepareNumber<int8_t>(location.address, operand, result, operation.kind);
    case sizeof(uint16_t):
        return unsignedValue ? PrepareNumber<uint16_t>(location.address, operand, result, operation.kind) : PrepareNumber<int16_t>(location.address, operand, result, operation.kind);
    case sizeof(uint32_t):
        return unsignedValue ? PrepareNumber<uint32_t>(location.address, operand, result, operation.kind) : PrepareNumber<int32_t>(location.address, operand, result, operation.kind);
    case sizeof(uint64_t):
        return unsignedValue ? PrepareNumber<uint64_t>(location.address, operand, result, operation.kind) : PrepareNumber<int64_t>(location.address, operand, result, operation.kind);
    default:
        throw std::runtime_error("unsupported integer width");
    }
}

// 在 CI-ready 阶段校验一个 patch 的 USER 根、字段路径、操作参数及可确定的类型布局。
// append 校验集合/key 及元素上下文，set 校验值，算术排除常量除零；不分配托管对象。
// 依赖运行时派生行的布局留到实际加载后检查，数值溢出也须结合那时的当前值判断。
void ValidatePatch(const ci::ProviderContext& context, const Patch& patch) {
    static const auto* userDataType = context.GetType("via.UserData");
    static const auto* stringType = context.GetType("System.String");
    if (!patch.rootType || !context.IsDerivedFrom(patch.rootType, userDataType))
        throw std::runtime_error("patch root must derive from via.UserData: " + patch.typeName);
    for (const auto& field : patch.fields) {
        const auto layout = ValidateFieldPath(context, patch.rootType, field.pathSteps);
        // 多态集合的实际成员在加载后才能确定；运行期仍须完整定位、准备和校验。
        if (!layout.type)
            continue;
        const auto& operation = field.operation;
        if (operation.kind == OperationKind::Append) {
            if (layout.valueSize || !operation.items.is_array())
                throw std::runtime_error("append requires a collection and items array");
            const auto collectionLayout = GetCollectionLayout(context, layout.type);
            if (collectionLayout.itemsField)
                GetListAddMethod(context, layout.type);
            const auto* elementType = collectionLayout.elementType;
            const auto elementSize = collectionLayout.valueSize;
            if (elementSize || elementType == stringType) {
                if (!operation.key.empty())
                    throw std::runtime_error("scalar append uses value identity and does not accept key");
            } else if (operation.key.empty()) {
                throw std::runtime_error("object append requires key");
            }
            ObjectBuilder builder(context, nullptr);
            for (const auto& item : operation.items) {
                ValidateJsonValue(context, elementType, elementSize, item, 0);
                // 身份先于构造解析，不传元素下标，防止位置值变成不稳定的去重 key。
                ReadDeclaredElementIdentity(context, builder, {.type = elementType, .valueSize = elementSize, .value = &item}, operation);
                if (elementSize || elementType == stringType)
                    continue;
                if (!item.is_object() || item.size() != 1)
                    throw std::runtime_error("object append items must declare their non-null concrete type");
                // ExcelUserData.cData[] 等基类集合允许派生行声明自己的身份字段。
                const auto* rowType = context.GetType(item.begin().key().c_str());
                if (operation.key == "$uri") {
                    static const auto get_URI = context.GetMethod(userDataType, "get_URI()");
                    if (!context.IsDerivedFrom(rowType, userDataType) || !get_URI)
                        throw std::runtime_error("$uri key requires USER elements and get_URI");
                } else {
                    const auto keyLayout = ValidateFieldPath(context, rowType, operation.keySteps);
                    if (keyLayout.type && (keyLayout.valueSize ? GetScalarSize(context, keyLayout.type) != keyLayout.valueSize : keyLayout.type != stringType))
                        throw std::runtime_error("object key must be scalar, Guid or string");
                }
            }
        } else if (operation.kind == OperationKind::Set) {
            if (layout.valueSize && GetSupportedValueSize(context, layout.type) != layout.valueSize)
                throw std::runtime_error("set supports scalar/Guid or reference; use member path for complex values");
            ValidateJsonValue(context, layout.type, layout.valueSize, operation.value);
        } else {
            if (!IsNumericType(context, layout.type) || !layout.valueSize)
                throw std::runtime_error("arithmetic requires a plain numeric field");
            ValidateJsonValue(context, layout.type, layout.valueSize, operation.value);
            alignas(std::max_align_t) uint8_t zero[sizeof(RE::SystemGuid)]{}, result[sizeof(RE::SystemGuid)]{};
            ObjectBuilder builder(context, nullptr);
            FieldLocation location{.address = zero, .type = layout.type, .valueSize = layout.valueSize};
            // 只在绑定期排除除零；其他计算依赖本次加载的真实当前值。
            if (operation.kind == OperationKind::Divide)
                PrepareArithmetic(context, builder, location, operation, result);
        }
    }
}

// set 和算术的字段写入结果；按目标字段 valueSize 选择标量字节或引用值，变化描述仅供日志。
struct FieldWriteResult {
    alignas(16) std::array<uint8_t, sizeof(RE::SystemGuid)> scalarBytes{};
    void* referenceValue{};
    std::string changeDescription;
};

// 新元素与去重身份成对移动，避免平行数组在跳过/异常分支中失配。
struct CollectionAppendItem {
    // 引用、标量及首批简单值共用16字节；按结构 Add 的间接参数要求对齐。
    alignas(16) std::array<uint8_t, sizeof(RE::SystemGuid)> elementBytes{};
    std::string identity;
};

// 集合追加的准备结果；保存原集合视图、新条目及待发布的替换集合，不包含字段定位状态。
struct CollectionAppendResult {
    CollectionView sourceCollection;
    void* listAddMethod{}; // 原生 List<T>.Add 地址；提交时按元素类型还原调用签名。
    std::vector<CollectionAppendItem> newItems;
    std::vector<std::string> skippedIdentities;
    void* replacementCollection{};
};

// 一条字段操作从定位到提交的状态；借用声明/Context，持有临时对象，并保存一种操作结果。
struct FieldOperationState {
    const ci::ProviderContext& context;
    const FieldOperation& fieldOperation;
    FieldLocation location;
    // 写值与追加互斥保存，普通写值不构造追加集合和身份状态。
    std::variant<FieldWriteResult, CollectionAppendResult> result;
    std::vector<void*> retainedObjects;

    // 借用上下文与字段操作声明；默认保存写入结果，追加操作在准备阶段切换结果类型。
    FieldOperationState(const ci::ProviderContext& context, const FieldOperation& fieldOperation)
        : context{context}, fieldOperation{fieldOperation} {}

    // 归还本条操作准备阶段临时持有的 owner、新行和待发布集合；异常退出同样执行。
    ~FieldOperationState() {
        for (auto* object : retainedObjects)
            context.Release(object);
    }
    // 在临时对象发布前持有一份引用，并记录给析构函数成对归还；空引用无需持有。
    void RetainObject(void* object) {
        if (object) {
            retainedObjects.push_back(object);
            context.Retain(object);
        }
    }
};

// 准备一次追加而不修改原集合：先读取原身份，再检查每个声明身份，跳过已有项。
// 仅为新项构造值，将最终物理下标传入构造器解析 $index；值与身份成对保存并临时保活。
// 已有 List 延迟原生 Add；数组或空 List 则先构造完整 replacement，稍后一次发布。
static void PrepareCollectionAppend(const ci::ProviderContext& context, void* vmContext, ObjectBuilder& builder, FieldOperationState& fieldState, std::vector<void*>& externalObjects) {
    static const auto* stringType = context.GetType("System.String");
    const auto& operation = fieldState.fieldOperation.operation;
    if (fieldState.location.valueSize)
        throw std::runtime_error("append target is not a reference");
    auto& appendResult = fieldState.result.emplace<CollectionAppendResult>();
    appendResult.sourceCollection = ReadCollection(context, fieldState.location.type, *static_cast<void**>(fieldState.location.address));
    if (appendResult.sourceCollection.list)
        appendResult.listAddMethod = GetListAddMethod(context, context.GetInstanceType(appendResult.sourceCollection.list));
    const auto& collection = appendResult.sourceCollection;
    if (collection.valueSize || collection.elementType == stringType) {
        if (!operation.key.empty())
            throw std::runtime_error("scalar append uses value identity and does not accept key");
    } else if (operation.key.empty()) {
        throw std::runtime_error("object append requires key");
    }
    std::set<std::string> existing;
    // 1. 只读原集合的身份索引；不会为了比较 key 复制已有行。
    for (size_t i = 0; i < collection.count; ++i) {
        const auto* address = collection.items->elements + i * collection.stride;
        if (!collection.valueSize && !*reinterpret_cast<void* const*>(address))
            continue;
        existing.insert(ReadElementIdentity(context, vmContext, collection, address, operation.key, operation.keySteps, externalObjects));
    }
    std::set<std::string> declared;
    // 2. 先从声明取身份再构造新元素，已有 key 不触发 USER 请求等构造副作用。
    for (const auto& item : operation.items) {
        auto identity = ReadDeclaredElementIdentity(context, builder, {.type = collection.elementType, .valueSize = collection.valueSize, .value = &item}, operation);
        if (!declared.insert(identity).second)
            throw std::runtime_error("append items contain duplicate business identity");
        if (existing.contains(identity)) {
            appendResult.skippedIdentities.push_back(std::move(identity));
            continue;
        }
        if (collection.count + appendResult.newItems.size() >= INT32_MAX)
            throw std::runtime_error("append count exceeds System.Int32 range");
        const auto elementIndex = int32_t(collection.count + appendResult.newItems.size());
        CollectionAppendItem candidate{.identity = std::move(identity)};
        const auto requestCount = builder.GetUserDataRequests().size();
        builder.WriteValue(nullptr, candidate.elementBytes.data(), collection.elementType, collection.valueSize, item, elementIndex);
        // builder 跨元素累计请求；这里只登记本次构造产生的尾段，不反复扫描旧请求。
        for (auto* request : builder.GetUserDataRequests().subspan(requestCount))
            if (std::ranges::find(externalObjects, request) == externalObjects.end())
                externalObjects.push_back(request);
        if (!collection.valueSize)
            fieldState.RetainObject(*reinterpret_cast<void**>(candidate.elementBytes.data()));
        appendResult.newItems.push_back(std::move(candidate));
    }
    if (appendResult.newItems.empty())
        return;
    // 3. 已有 List 留到提交时调用原生 Add；数组/空 List 先拼好替换集合但不写根字段。
    if (collection.list)
        return; // 已有 List 在提交阶段复用原生 Add。
    auto* array = static_cast<RE::SystemArray<uint8_t>*>(context.CreateArray(vmContext, collection.elementType, collection.count + appendResult.newItems.size(), false));
    if (!array || array == vmContext || array->contained_type != collection.elementType || array->count != collection.count + appendResult.newItems.size())
        throw std::runtime_error("append array allocation/type contract failed");
    if (collection.count)
        memcpy(array->elements, collection.items->elements, collection.count * collection.stride);
    for (size_t i = 0; i < appendResult.newItems.size(); ++i)
        memcpy(array->elements + (collection.count + i) * collection.stride, appendResult.newItems[i].elementBytes.data(), collection.stride);
    if (fieldState.location.type->objectType == std::to_underlying(via::clr::VMObjType::Array)) {
        appendResult.replacementCollection = array;
    } else {
        // null List：构造完整集合后一次发布，不在根对象上暴露半成品。
        auto* list = context.CreateInstance(vmContext, fieldState.location.type, false);
        const auto* itemsField = context.GetFieldLayout(fieldState.location.type, "_items");
        const auto* sizeField = context.GetFieldLayout(fieldState.location.type, "_size");
        if (!list || !context.AssignReference(list, context.GetField<void*>(list, itemsField), array))
            throw std::runtime_error("new List items assignment failed");
        *context.GetField<int32_t>(list, sizeField) = int32_t(array->count);
        appendResult.replacementCollection = list;
    }
    fieldState.RetainObject(appendResult.replacementCollection); // 完整填充后才首次 retain。
}

// 把元素暂存字节还原为真实 C++ 参数类型，再按原生 Add 的对应 ABI 调用。
// 避免把浮点、Guid 或窄整数一律当成对象指针传参。
template <class T>
static void CallListAdd(void* addMethod, void* vmContext, void* list, const void* value) {
    using FnAdd = void (*)(void*, void*, T);
    T scalar;
    memcpy(&scalar, value, sizeof(scalar));
    reinterpret_cast<FnAdd>(addMethod)(vmContext, list, scalar);
}

// 提交已有 List 的准备条目；小纯值按地址传参，其余按引用/浮点/Guid/整数分派。
// 容量、版本及已有 List 的原生行为交给 Add，不自行改写其内部增长逻辑。
static void CommitListAppend(const ci::ProviderContext& context, void* vmContext, const CollectionAppendResult& appendResult) {
    static const auto* singleType = context.GetType("System.Single");
    static const auto* doubleType = context.GetType("System.Double");
    static const auto* guidType = context.GetType("System.Guid");
    const auto& collection = appendResult.sourceCollection;
    for (const auto& item : appendResult.newItems) {
        if (!collection.valueSize) {
            CallListAdd<void*>(appendResult.listAddMethod, vmContext, collection.list, item.elementBytes.data());
        } else if (FindSupportedValueLayout(context, collection.elementType)) {
            // 当前原生 List<Color>/vec3/Quaternion.Add 均从 R8 指针读取值，Color 也不是整数寄存器传值。
            using FnAddValue = void (*)(void*, void*, const void*);
            reinterpret_cast<FnAddValue>(appendResult.listAddMethod)(vmContext, collection.list, item.elementBytes.data());
        } else if (collection.elementType == singleType) {
            CallListAdd<float>(appendResult.listAddMethod, vmContext, collection.list, item.elementBytes.data());
        } else if (collection.elementType == doubleType) {
            CallListAdd<double>(appendResult.listAddMethod, vmContext, collection.list, item.elementBytes.data());
        } else if (collection.elementType == guidType) {
            CallListAdd<RE::SystemGuid>(appendResult.listAddMethod, vmContext, collection.list, item.elementBytes.data());
        } else {
            switch (collection.valueSize) {
            case sizeof(uint8_t):
                CallListAdd<uint8_t>(appendResult.listAddMethod, vmContext, collection.list, item.elementBytes.data());
                break;
            case sizeof(uint16_t):
                CallListAdd<uint16_t>(appendResult.listAddMethod, vmContext, collection.list, item.elementBytes.data());
                break;
            case sizeof(uint32_t):
                CallListAdd<uint32_t>(appendResult.listAddMethod, vmContext, collection.list, item.elementBytes.data());
                break;
            case sizeof(uint64_t):
                CallListAdd<uint64_t>(appendResult.listAddMethod, vmContext, collection.list, item.elementBytes.data());
                break;
            default:
                throw std::runtime_error("unsupported List element ABI");
            }
        }
    }
}

// 判断两个非空地址区间是否相交；使用地址差而不是地址加长度，避免加法溢出。
static bool RangesOverlap(const void* left, size_t leftSize, const void* right, size_t rightSize) {
    if (!leftSize || !rightSize)
        return false;
    const auto leftAddress = reinterpret_cast<uintptr_t>(left);
    const auto rightAddress = reinterpret_cast<uintptr_t>(right);
    return leftAddress <= rightAddress ? rightAddress - leftAddress < leftSize : leftAddress - rightAddress < rightSize;
}

// 判断追加会触碰的 List 状态或已有数组元素区是否与另一个字段路径重叠。
// 在初始定位后执行，防止追加改变集合状态或已有元素区，使另一条已定位路径失效。
static bool AppendOverlapsLocation(const ci::ProviderContext& context, const FieldOperationState& fieldState, const FieldLocation& other) {
    if (fieldState.fieldOperation.operation.kind != OperationKind::Append)
        return false;
    if (fieldState.location.valueSize)
        throw std::runtime_error("append target is not a reference");
    const auto collection = ReadCollection(context, fieldState.location.type, *static_cast<void**>(fieldState.location.address));
    if (collection.list) {
        const auto* type = context.GetInstanceType(collection.list);
        for (const auto* fieldName : {"_items", "_size", "_version"}) {
            const auto* field = context.GetFieldLayout(type, fieldName);
            auto* address = field ? context.GetField(collection.list, field) : nullptr;
            if (address && (RangesOverlap(address, field->valueSize ? field->valueSize : sizeof(void*), other.address, other.valueSize ? other.valueSize : sizeof(void*)) || std::ranges::find(other.ancestorReferenceSlots, address) != other.ancestorReferenceSlots.end()))
                return true;
        }
    }
    if (!collection.items)
        return false;
    if (other.owner == collection.items || RangesOverlap(collection.items->elements, collection.count * collection.stride, other.address, other.valueSize ? other.valueSize : sizeof(void*)))
        return true;
    return std::ranges::any_of(other.ancestorReferenceSlots, [&](const void* address) {
        return RangesOverlap(collection.items->elements, collection.count * collection.stride, address, sizeof(void*));
    });
}

// 在构造结果前比较同批字段槽、祖先引用及追加目标，拒绝别名或父子写入冲突。
// 依赖先后顺序的修改应拆成有序 patch，不能依赖字段字典的遍历顺序。
static void ValidateIndependentLocations(const ci::ProviderContext& context, const std::vector<std::unique_ptr<FieldOperationState>>& fieldStates) {
    for (size_t i = 0; i < fieldStates.size(); ++i) {
        const auto& current = *fieldStates[i];
        for (size_t j = 0; j < i; ++j) {
            const auto& previous = *fieldStates[j];
            const auto aSize = current.location.valueSize ? current.location.valueSize : sizeof(void*);
            const auto bSize = previous.location.valueSize ? previous.location.valueSize : sizeof(void*);
            bool overlap = RangesOverlap(current.location.address, aSize, previous.location.address, bSize) ||
                           std::ranges::find(current.location.ancestorReferenceSlots, previous.location.address) != current.location.ancestorReferenceSlots.end() ||
                           std::ranges::find(previous.location.ancestorReferenceSlots, current.location.address) != previous.location.ancestorReferenceSlots.end() ||
                           AppendOverlapsLocation(context, current, previous.location) || AppendOverlapsLocation(context, previous, current.location);
            if (current.fieldOperation.operation.kind == OperationKind::Append && previous.fieldOperation.operation.kind == OperationKind::Append) {
                const auto* aObject = *static_cast<void* const*>(current.location.address);
                const auto* bObject = *static_cast<void* const*>(previous.location.address);
                overlap = overlap || (aObject && aObject == bObject);
            }
            if (overlap)
                throw std::runtime_error("patch paths alias or overlap; split into ordered patches: " + current.fieldOperation.fieldPath + " / " + previous.fieldOperation.fieldPath);
        }
    }
}

// 释放跨 patch 保留的外部 USER 请求和来源索引 owner，结束本次根对象执行状态。
PatchExecutor::~PatchExecutor() {
    for (auto* request : createdUserDataRequests)
        context.Release(request);
    for (auto* owner : sourceOwners)
        context.Release(owner);
}

// 为来源索引中的真实 owner 去重持有，防止父引用被替换后旧地址在本轮被重用。
void PatchExecutor::RetainSourceOwner(void* owner) {
    if (owner && std::ranges::find(sourceOwners, owner) == sourceOwners.end()) {
        sourceOwners.push_back(owner);
        context.Retain(owner);
    }
}

// 执行一个独立批次：整批定位并检查别名，准备所有写值/追加结果，再按声明提交。
// 准备阶段只写暂存区或新对象；可请求资源，但不改原根字段。临时持有随准备记录析构归还。
// 提交阶段维护跨文件冲突来源；中途失败明确报告已提交数量，不声称可回滚此前写入。
void PatchExecutor::ApplyPatch(const Patch& patch, const std::string& source) {
    if (context.GetInstanceType(root) != patch.rootType)
        throw std::runtime_error("patch runtime root type mismatch: " + patch.typeName);
    ObjectBuilder builder(context, vmContext);
    std::vector<void*> blockedObjects(externalObjects.begin(), externalObjects.end());
    blockedObjects.insert(blockedObjects.end(), createdUserDataRequests.begin(), createdUserDataRequests.end());
    std::vector<std::unique_ptr<FieldOperationState>> fieldStates;
    fieldStates.reserve(patch.fields.size());
    // 1. 全批先定位并临时持有实际 owner，不让字段字典顺序决定能否读到某个父对象。
    for (const auto& field : patch.fields) {
        auto fieldState = std::make_unique<FieldOperationState>(context, field);
        try {
            fieldState->location = ResolveFieldPath(context, vmContext, root, field.pathSteps, blockedObjects);
        } catch (const std::exception& error) {
            throw std::runtime_error("locate field=" + field.fieldPath + ": " + error.what());
        }
        fieldState->RetainObject(fieldState->location.owner);
        fieldStates.push_back(std::move(fieldState));
    }
    // 2. 在创建结果前拒绝同批别名/父子写入，依赖顺序的修改必须拆成多个 patch。
    ValidateIndependentLocations(context, fieldStates);
    // 3. 准备互斥的写值或追加结果；这一阶段的失败只释放暂存与临时持有，不提交根字段。
    for (auto& fieldState : fieldStates) {
        const auto& operation = fieldState->fieldOperation.operation;
        try {
            if (operation.kind == OperationKind::Append) {
                PrepareCollectionAppend(context, vmContext, builder, *fieldState, blockedObjects);
            } else {
                auto& writeResult = std::get<FieldWriteResult>(fieldState->result);
                if (operation.kind == OperationKind::Set) {
                    if (fieldState->location.valueSize) {
                        if (GetSupportedValueSize(context, fieldState->location.type) != fieldState->location.valueSize || fieldState->location.valueSize > writeResult.scalarBytes.size())
                            throw std::runtime_error("set value layout is unsupported");
                        builder.WriteValue(nullptr, writeResult.scalarBytes.data(), fieldState->location.type, fieldState->location.valueSize, operation.value);
                    } else {
                        writeResult.referenceValue = builder.CreateObject(fieldState->location.type, operation.value);
                        fieldState->RetainObject(writeResult.referenceValue);
                    }
                } else {
                    writeResult.changeDescription = PrepareArithmetic(context, builder, fieldState->location, operation, writeResult.scalarBytes.data());
                }
            }
        } catch (const std::exception& error) {
            throw std::runtime_error("prepare field=" + fieldState->fieldOperation.fieldPath + " op=" + GetOperationName(operation.kind) + ": " + error.what());
        }
    }
    // 4. 将外部请求的持有交接到本次根执行器，后续 patch 仍不得穿越其未解码字段。
    const auto requests = builder.GetUserDataRequests();
    for (auto* request : requests) {
        if (std::ranges::find(createdUserDataRequests, request) == createdUserDataRequests.end()) {
            createdUserDataRequests.push_back(request);
            context.Retain(request); // 保留新建外部请求，阻止后续 patch 穿越尚未解码的字段。
        }
    }
    // 来源索引保留真实 owner，父引用替换后旧地址也不在本次执行期间被重用。
    for (const auto& fieldState : fieldStates) {
        if (fieldState->fieldOperation.operation.kind == OperationKind::Set || fieldState->fieldOperation.operation.kind == OperationKind::Append)
            RetainSourceOwner(fieldState->location.owner);
        if (fieldState->fieldOperation.operation.kind == OperationKind::Append && fieldState->location.type->objectType != std::to_underlying(via::clr::VMObjType::Array)) {
            const auto& appendResult = std::get<CollectionAppendResult>(fieldState->result);
            RetainSourceOwner(appendResult.sourceCollection.list ? appendResult.sourceCollection.list : appendResult.replacementCollection);
        }
    }
    // 5. 所有准备成功后才发布结果；记录来源冲突，异常时报告已完成条数而不是假装回滚。
    size_t committed = 0;
    try {
        for (const auto& fieldState : fieldStates) {
            const auto& operation = fieldState->fieldOperation.operation;
            if (operation.kind == OperationKind::Append) {
                const auto& appendResult = std::get<CollectionAppendResult>(fieldState->result);
                const auto* identityOwner = fieldState->location.type->objectType == std::to_underlying(via::clr::VMObjType::Array) ? fieldState->location.address : (appendResult.sourceCollection.list ? appendResult.sourceCollection.list : appendResult.replacementCollection);
                for (const auto& identity : appendResult.skippedIdentities) {
                    const auto previous = appendedSources.find({identityOwner, identity});
                    if (previous != appendedSources.end() && previous->second != source)
                        _MESSAGE("[UserDataExtension] Conflict append field=%s previous=%s current=%s; existing item retained", fieldState->fieldOperation.fieldPath.c_str(), previous->second.c_str(), source.c_str());
                }
                if (!appendResult.newItems.empty()) {
                    if (appendResult.sourceCollection.list)
                        CommitListAppend(context, vmContext, appendResult);
                    else if (!context.AssignReference(fieldState->location.owner, static_cast<void**>(fieldState->location.address), appendResult.replacementCollection))
                        throw std::runtime_error("append collection publication failed");
                    for (const auto& added : appendResult.newItems)
                        appendedSources[{identityOwner, added.identity}] = source;
                }
                _MESSAGE("[UserDataExtension] Applied source=%s field=%s op=append added=%zu skipped=%zu", source.c_str(), fieldState->fieldOperation.fieldPath.c_str(), appendResult.newItems.size(), appendResult.skippedIdentities.size());
            } else {
                const auto& writeResult = std::get<FieldWriteResult>(fieldState->result);
                if (operation.kind == OperationKind::Set) {
                    const auto previous = assignedSources.find(fieldState->location.address);
                    if (previous != assignedSources.end() && previous->second != source)
                        _MESSAGE("[UserDataExtension] Conflict set field=%s previous=%s current=%s; last value wins", fieldState->fieldOperation.fieldPath.c_str(), previous->second.c_str(), source.c_str());
                }
                if (fieldState->location.valueSize) {
                    memcpy(fieldState->location.address, writeResult.scalarBytes.data(), fieldState->location.valueSize);
                } else if (!context.AssignReference(fieldState->location.owner, static_cast<void**>(fieldState->location.address), writeResult.referenceValue)) {
                    throw std::runtime_error("set reference publication failed");
                }
                if (operation.kind == OperationKind::Set)
                    assignedSources[fieldState->location.address] = source;
                _MESSAGE("[UserDataExtension] Applied source=%s field=%s op=%s %s", source.c_str(), fieldState->fieldOperation.fieldPath.c_str(), GetOperationName(operation.kind), writeResult.changeDescription.c_str());
            }
            ++committed;
        }
    } catch (const std::exception& error) {
        const auto& failed = fieldStates[committed]->fieldOperation;
        throw std::runtime_error("commit partial operations=" + std::to_string(committed) + " field=" + failed.fieldPath + " op=" + GetOperationName(failed.operation.kind) + ": " + error.what());
    }
}
} // namespace UserDataExtension
