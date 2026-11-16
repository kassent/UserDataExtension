#include "UserDataCollection.h"
#include "UserDataObjectBuilder.h"
#include <stdexcept>
#include <utility>

namespace UserDataExtension {
// 解析一维数组或 List 的元素类型、步长及存储字段，供构造、选择和追加共用。
// 值元素仅接受已核实的标量及首批小纯值结构；布局不符时抛异常，不猜测步长。
CollectionLayout GetCollectionLayout(const ci::SDKAPI& context, ci::TypeDefinitionHandle type) {
    if (!type)
        throw std::runtime_error("collection type is missing");
    static const auto* int32Type = context.tdb->find_type("System.Int32");
    CollectionLayout layout;
    const auto* arrayType = type;
    if (!context.type_definition->is_array(type)) {
        layout.itemsField = ci::API::TypeDefinition{&context, type}.find_field("_items");
        layout.sizeField = ci::API::TypeDefinition{&context, type}.find_field("_size");
        if (!layout.itemsField || layout.itemsField.is_static() || layout.itemsField.get_type().get_valuetype_size() ||
            !layout.sizeField || layout.sizeField.is_static() || layout.sizeField.get_type().handle != int32Type || layout.sizeField.get_type().get_valuetype_size() != sizeof(int32_t))
            throw std::runtime_error("collection requires an array or List layout");
        arrayType = layout.itemsField.get_type().handle;
    }
    if (!arrayType || !context.type_definition->is_array(arrayType) || !context.type_definition->get_element_type(arrayType))
        throw std::runtime_error("collection array/element type is missing");
    // TDB81 +0x2C 的高三位是数组维数；CI 的数组类型名重建也使用此字段。
    if (context.type_definition->get_array_rank(arrayType) > 1)
        throw std::runtime_error("only one-dimensional collections are supported");
    layout.elementType = context.type_definition->get_element_type(arrayType);
    layout.valueSize = context.type_definition->is_valuetype(layout.elementType) ? GetSupportedValueSize(context, layout.elementType) : 0;
    layout.stride = layout.valueSize ? layout.valueSize : sizeof(void*);
    return layout;
}

// 读取集合的借用视图，不分配对象或增加引用；空集合引用返回 count=0 的类型视图。
// List 的有效长度取 _size 而非底层数组容量，并核对运行时类型、容量及元素类型。
CollectionView ReadCollection(const ci::SDKAPI& context, ci::TypeDefinitionHandle expectedType, void* collection) {
    const auto layout = GetCollectionLayout(context, expectedType);
    CollectionView result{.elementType = layout.elementType, .valueSize = layout.valueSize, .stride = layout.stride};
    if (!collection)
        return result;
    const auto* actualType = context.managed_object->get_type_definition(collection);
    if (!context.type_definition->is_derived_from(actualType, expectedType))
        throw std::runtime_error("collection runtime type mismatch");
    const auto actualLayout = GetCollectionLayout(context, actualType);
    if (actualLayout.itemsField) {
        result.list = collection;
        result.items = actualLayout.itemsField.get_data<RE::SystemArray<uint8_t>*>(collection);
        const auto count = actualLayout.sizeField.get_data<int32_t>(collection);
        if (count < 0 || (!result.items && count) || (result.items && uint32_t(count) > result.items->count))
            throw std::runtime_error("List size/capacity contract failed");
        result.count = size_t(count);
    } else {
        result.items = static_cast<RE::SystemArray<uint8_t>*>(collection);
        result.count = result.items->count;
    }
    if (result.items) {
        const auto arrayType = context.managed_object->get_type_definition(result.items);
        if (!context.type_definition->is_array(arrayType) ||
            context.type_definition->get_element_type(arrayType) != result.elementType)
            throw std::runtime_error("collection element type mismatch");
    }
    return result;
}

// 沿相同存储布局的父链找到 List 的 Add 声明，返回供提交阶段调用的入口地址。
// 拒绝派生类型遮蔽存储字段，避免同名重载或不同布局改变实际参数 ABI。
void* GetListAddMethod(const ci::SDKAPI& context, ci::TypeDefinitionHandle type) {
    const auto layout = GetCollectionLayout(context, type);
    while (const auto* parent = context.type_definition->get_parent_type(type)) {
        const auto parentItems = ci::API::TypeDefinition{&context, parent}.find_field("_items");
        const auto parentSize = ci::API::TypeDefinition{&context, parent}.find_field("_size");
        if (!parentItems || parentItems.is_static() || !parentSize || parentSize.is_static())
            break;
        if (parentItems.get_type().handle != layout.itemsField.get_type().handle || parentItems.get_offset_from_fieldptr() != layout.itemsField.get_offset_from_fieldptr() || parentSize.get_type().handle != layout.sizeField.get_type().handle || parentSize.get_offset_from_fieldptr() != layout.sizeField.get_offset_from_fieldptr())
            throw std::runtime_error("List storage fields are shadowed by a derived type");
        type = parent;
    }
    // 从声明存储的 List 基类取 Add，避免派生类同名重载改变参数 ABI。
    auto* Add = ci::API::TypeDefinition{&context, type}.find_method("Add").get_function();
    if (!Add)
        throw std::runtime_error("List.Add entry missing");
    return Add;
}
} // namespace UserDataExtension
