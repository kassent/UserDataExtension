#pragma once
#include "UserDataConfiguration.h"
#include <ci/API.hpp>
#include <map>
#include <span>
#include <string>

namespace UserDataExtension {
// 元数据阶段校验声明、字段布局与操作参数，不执行资源请求或托管对象分配。
void ValidateTypePatch(const ci::SDKAPI& context, const TypePatch& typePatch);

// 一个已解码根对象的局部执行状态；不跨资源或跨次解码保留地址和身份索引。
class PatchExecutor {
  public:
    // 借用一个已解码根及原生调用上下文；外部 USER 列表用于本轮路径边界检查。
    PatchExecutor(const ci::SDKAPI& context, void* vmContext, void* root, std::span<void* const> externalObjects = {})
        : context{context}, vmContext{vmContext}, root{root}, externalObjects{externalObjects} {}
    // 归还跨 patch 保存的请求与来源 owner；不销毁借用的根对象或 Context。
    ~PatchExecutor();
    // 禁用复制，避免复制执行状态后重复归还持有的对象。
    PatchExecutor(const PatchExecutor&) = delete;
    // 禁用复制赋值，保持单一根对象执行状态及引用归属。
    PatchExecutor& operator=(const PatchExecutor&) = delete;
    // 定位并准备整批结果后提交，跨 patch 记录冲突来源；失败抛异常。
    // 准备失败不改原根字段，提交中失败保留已完成部分并报告位置，不提供回滚。
    void ApplyTypePatch(const TypePatch& typePatch, const std::string& source);

  private:
    const ci::SDKAPI& context;
    void* vmContext;
    void* root;
    std::span<void* const> externalObjects;
    std::vector<void*> createdUserDataRequests;
    std::vector<void*> sourceOwners;
    std::map<const void*, std::string> assignedSources;
    std::map<std::pair<const void*, std::string>, std::string> appendedSources;
    // 按实际 owner 去重持有，保证冲突来源索引中的旧地址在本轮执行期间不会被重用。
    void RetainSourceOwner(void* owner);
};
} // namespace UserDataExtension
