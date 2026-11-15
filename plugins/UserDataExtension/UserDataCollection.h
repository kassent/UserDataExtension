#pragma once
#include "ClassProvider.h"
#include <GameTypes.h>

namespace UserDataExtension {
// 只读类型布局；List 通过 itemsField/sizeField 关联其数组存储，数组本身没有这两个字段。
struct CollectionLayout {
    const ci::FieldLayout* itemsField{};
    const ci::FieldLayout* sizeField{};
    const ci::Type* elementType{};
    uint32_t valueSize{};
    uint32_t stride{};
};

// 当前集合的借用快照；count 是有效元素数，items->count 可能只是 List 的底层容量。
struct CollectionView {
    void* list{};
    RE::SystemArray<uint8_t>* items{};
    const ci::Type* elementType{};
    uint32_t valueSize{};
    uint32_t stride{};
    size_t count{};
};

// 解析已核实的一维数组/List 布局；失败抛异常，构造/选择/追加共用该约束。
CollectionLayout GetCollectionLayout(const ci::ProviderContext& context, const ci::Type* type);
// 读取借用的集合视图并核对长度与元素类型；空引用返回合法的零长度类型视图。
CollectionView ReadCollection(const ci::ProviderContext& context, const ci::Type* expectedType, void* collection);
// 查找存储声明方的原生 List.Add 入口，拒绝派生存储遮蔽导致的参数 ABI 歧义。
void* GetListAddMethod(const ci::ProviderContext& context, const ci::Type* type);
} // namespace UserDataExtension
