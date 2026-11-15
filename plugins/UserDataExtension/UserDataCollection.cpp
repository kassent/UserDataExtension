#include "UserDataCollection.h"
#include "UserDataObjectBuilder.h"
#include <stdexcept>
#include <utility>

namespace UserDataExtension {
// 解析一维数组或 List 的元素类型、步长及存储字段，供构造、选择和追加共用。
// 值元素仅接受已核实的标量及首批小纯值结构；布局不符时抛异常，不猜测步长。
CollectionLayout GetCollectionLayout(const ci::ProviderContext& context, const ci::Type* type) {
    if (!type)
        throw std::runtime_error("collection type is missing");
    static const auto* int32Type = context.GetType("System.Int32");
    CollectionLayout layout;
    const auto* arrayType = type;
    if (type->objectType != std::to_underlying(via::clr::VMObjType::Array)) {
        layout.itemsField = context.GetFieldLayout(type, "_items");
        layout.sizeField = context.GetFieldLayout(type, "_size");
        if (!layout.itemsField || layout.itemsField->valueSize || !layout.sizeField || layout.sizeField->type != int32Type || layout.sizeField->valueSize != sizeof(int32_t))
            throw std::runtime_error("collection requires an array or List layout");
        arrayType = layout.itemsField->type;
    }
    if (!arrayType || arrayType->objectType != std::to_underlying(via::clr::VMObjType::Array) || !arrayType->elementTypeIndex)
        throw std::runtime_error("collection array/element type is missing");
    // TDB81 +0x2C 的高三位是数组维数；CI 的数组类型名重建也使用此字段。
    if (arrayType->arrayRank > 1)
        throw std::runtime_error("only one-dimensional collections are supported");
    layout.elementType = arrayType - arrayType->index + arrayType->elementTypeIndex;
    layout.valueSize = layout.elementType->objectType == std::to_underlying(via::clr::VMObjType::ValType) ? GetSupportedValueSize(context, layout.elementType) : 0;
    layout.stride = layout.valueSize ? layout.valueSize : sizeof(void*);
    return layout;
}

// 读取集合的借用视图，不分配对象或增加引用；空集合引用返回 count=0 的类型视图。
// List 的有效长度取 _size 而非底层数组容量，并核对运行时类型、容量及元素类型。
CollectionView ReadCollection(const ci::ProviderContext& context, const ci::Type* expectedType, void* collection) {
    const auto layout = GetCollectionLayout(context, expectedType);
    CollectionView result{.elementType = layout.elementType, .valueSize = layout.valueSize, .stride = layout.stride};
    if (!collection)
        return result;
    const auto* actualType = context.GetInstanceType(collection);
    if (!context.IsDerivedFrom(actualType, expectedType))
        throw std::runtime_error("collection runtime type mismatch");
    const auto actualLayout = GetCollectionLayout(context, actualType);
    if (actualLayout.itemsField) {
        result.list = collection;
        result.items = *context.GetField<RE::SystemArray<uint8_t>*>(collection, actualLayout.itemsField);
        const auto count = *context.GetField<int32_t>(collection, actualLayout.sizeField);
        if (count < 0 || (!result.items && count) || (result.items && uint32_t(count) > result.items->count))
            throw std::runtime_error("List size/capacity contract failed");
        result.count = size_t(count);
    } else {
        result.items = static_cast<RE::SystemArray<uint8_t>*>(collection);
        result.count = result.items->count;
    }
    if (result.items && result.items->contained_type != result.elementType)
        throw std::runtime_error("collection element type mismatch");
    return result;
}

// 沿相同存储布局的父链找到 List 的 Add 声明，返回供提交阶段调用的入口地址。
// 拒绝派生类型遮蔽存储字段，避免同名重载或不同布局改变实际参数 ABI。
void* GetListAddMethod(const ci::ProviderContext& context, const ci::Type* type) {
    const auto layout = GetCollectionLayout(context, type);
    const auto* types = type - type->index;
    while (type->parentTypeIndex) {
        const auto* parent = types + type->parentTypeIndex;
        const auto* parentItems = context.GetFieldLayout(parent, "_items");
        const auto* parentSize = context.GetFieldLayout(parent, "_size");
        if (!parentItems || !parentSize)
            break;
        if (parentItems->type != layout.itemsField->type || parentItems->valueOffset != layout.itemsField->valueOffset || parentSize->type != layout.sizeField->type || parentSize->valueOffset != layout.sizeField->valueOffset)
            throw std::runtime_error("List storage fields are shadowed by a derived type");
        type = parent;
    }
    // 从声明存储的 List 基类取 Add，避免派生类同名重载改变参数 ABI。
    auto* Add = context.GetMethod(type, "Add");
    if (!Add)
        throw std::runtime_error("List.Add entry missing");
    return Add;
}
} // namespace UserDataExtension
