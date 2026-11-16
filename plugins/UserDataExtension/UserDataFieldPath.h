#pragma once
#include <ci/API.hpp>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace UserDataExtension {
enum class FieldPathStepKind {
    Field,
    Index,
    Match,
};

// 登记时解析的路径语法；同一步只使用 kind 对应的字段名、下标或匹配描述。
struct FieldPathStep {
    FieldPathStepKind kind{};
    std::string fieldName;
    size_t index{};
    std::string key;
    std::vector<FieldPathStep> keySteps;
    nlohmann::json value;
};

// 当前原生解码调用内有效的字段槽；owner 是引用写屏障的实际宿主，不一定是 USER 根。
struct FieldLocation {
    void* owner{};
    void* address{};
    ci::TypeDefinitionHandle type{};
    uint32_t valueSize{};
    // 经过的引用槽地址；用于检测同批次父引用替换与子字段写入冲突。
    std::vector<void*> ancestorReferenceSlots;
};

// 只解析路径语法为字段/下标/匹配步骤；非法或不完整路径抛异常，不访问对象。
std::vector<FieldPathStep> ParseFieldPath(std::string_view path);
// 仅用元数据校验路径并返回末端类型；nullptr 表示须待实际派生元素检查。
ci::TypeDefinitionHandle ValidateFieldPath(const ci::SDKAPI& context, ci::TypeDefinitionHandle rootType, std::span<const FieldPathStep> steps);
// 在当前已解码对象图上定位借用字段槽及真实 owner；保留祖先槽用于批次冲突检测。
// 拒绝跨入 externalObjects 中的未解码 USER，不自动创建缺失父对象或等待资源。
FieldLocation ResolveFieldPath(const ci::SDKAPI& context, void* vmContext, void* root, std::span<const FieldPathStep> steps, std::span<void* const> externalObjects = {});
} // namespace UserDataExtension
