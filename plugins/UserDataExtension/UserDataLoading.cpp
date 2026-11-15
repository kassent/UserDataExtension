#include "UserDataLoading.h"
#include "UserDataObjectBuilder.h"
#include "UserDataPatchExecutor.h"
#include <GameTypes.h>
#include <HookUtils.h>
#include <LogUtils.h>
#include <algorithm>
#include <stdexcept>

namespace UserDataExtension {
using Fnget_URI = RE::SystemString* (*)(void*, void*);

// 对一个已解码根按文件、extension、patch 的既定顺序应用 URI 与根类型均匹配的声明。
// 同一执行器跨 patch 保留冲突来源；一条 patch 失败即停止此根的后续应用，已提交部分不回滚。
static void ApplyDecodedRoot(const ci::ProviderContext& context, void* vmContext, void* root, const ci::Type* rootType, const std::string& uri, std::span<void* const> externalObjects, const std::vector<ConfigurationDeclaration>& declarations) {
    PatchExecutor executor(context, vmContext, root, externalObjects);
    size_t applied = 0;
    for (const auto& file : declarations) {
        const auto source = GetConfigurationSource(file);
        for (const auto& extension : file.extensions) {
            if (extension.target != uri)
                continue;
            for (size_t index = 0; index < extension.patches.size(); ++index) {
                const auto& patch = extension.patches[index];
                if (rootType != patch.rootType)
                    continue;
                try {
                    executor.ApplyPatch(patch, source);
                    ++applied;
                } catch (const std::exception& error) {
                    _MESSAGE("[UserDataExtension] stopped USER=%s source=%s patch=%zu type=%s after=%zu reason=%s", uri.c_str(), source.c_str(), index, patch.typeName.c_str(), applied, error.what());
                    return;
                }
            }
        }
    }
    if (applied)
        _MESSAGE("[UserDataExtension] extended USER=%s patches=%zu", uri.c_str(), applied);
}

// 在 CI-ready 阶段绑定类型并验证每份文件，收集监听根类型，再安装解码完成出口 hook。
// 无有效计划时直接成功；有效计划与 Context 由进程期静态存储交给原生回调，不另建调度器。
// 回调沿用当前原生托管作用域，在 onLoad 前扩展完整对象图，跳过尚未解码的外部 USER。
bool InstallLoadingHook(const ci::ProviderContext* context, std::vector<ConfigurationDeclaration> declarations) {
    if (!context || !context->GetType || !context->GetMethod || !context->GetFieldLayout || !context->GetInstanceType || !context->GetRawField || !context->CreateInstance || !context->CreateArray || !context->CreateString || !context->Retain || !context->Release || !context->AssignReference || !context->DeserializeRsz || !context->IsDerivedFrom || !context->IsEnum || !context->GetValueTypeSize)
        throw std::runtime_error("required CI Context services are missing");
    static const auto* userDataType = context->GetType("via.UserData");
    std::vector<ConfigurationDeclaration> validated;
    std::vector<const ci::Type*> rootTypes;
    // 先整份绑定再接收，避免同一 JSON 中前半有效、后半失败却留下部分计划。
    for (auto& file : declarations) {
        try {
            for (auto& extension : file.extensions) {
                for (auto& patch : extension.patches) {
                    patch.rootType = context->GetType(patch.typeName.c_str());
                    if (!context->IsDerivedFrom(patch.rootType, userDataType))
                        throw std::runtime_error("patch root is not a via.UserData type: " + patch.typeName);
                    ValidatePatch(*context, patch);
                }
            }
            validated.push_back(std::move(file));
        } catch (const std::exception& error) {
            _MESSAGE("[UserDataExtension] rejected source=%s reason=%s", GetConfigurationSource(file).c_str(), error.what());
        }
    }
    for (const auto& file : validated)
        for (const auto& extension : file.extensions)
            for (const auto& patch : extension.patches)
                if (std::ranges::find(rootTypes, patch.rootType) == rootTypes.end())
                    rootTypes.push_back(patch.rootType);
    if (rootTypes.empty()) {
        _MESSAGE("[UserDataExtension] no valid configuration; loading hook is not needed");
        return true;
    }
    static const auto get_URI = reinterpret_cast<Fnget_URI>(context->GetMethod(userDataType, "get_URI()"));
    if (!get_URI)
        throw std::runtime_error("via.UserData.get_URI() entry missing");
    // 已验证的原生出口：内部图解码完成，尚在原托管作用域内，onLoad 尚未执行。
    const auto DeserializeRszCompletedGraphExit = HookUtils::LookupFunctionPattern(reinterpret_cast<void*>(context->DeserializeRsz), "48 63 43 08 48 39 C7 7C ? 48 89 F1 31 D2 E8 ? ? ? ? 8B 46 7C FF C8 89 46 7C", 0x700);
    if (!DeserializeRszCompletedGraphExit)
        throw std::runtime_error("native RSZ completed-graph exit contract missing");
    static const ci::ProviderContext* const ciContext = context;
    static const auto executionPlan = std::move(validated);
    static const auto watchedTypes = std::move(rootTypes);
    // 同步接在原解码调用内：读取本次实例表，过滤外部占位对象，再处理命中的根。
    static auto DeserializeRszCompletedGraphHook = safetyhook::create_mid(reinterpret_cast<void*>(DeserializeRszCompletedGraphExit + 9), [](SafetyHookContext& registers) {
        try {
            const auto& context = *ciContext;
            // 此已核出口中 rsi 为 vmctx，栈 +0x50 为实例数组，+0x38 为外部实例位图指针。
            // 表中既有本图已解码对象，也有未解码 USER 请求，不能对所有实例直接合并。
            const auto* instances = reinterpret_cast<const ci::RszObjectArray*>(registers.rsp + 0x50);
            const auto* externalInstanceBits = *reinterpret_cast<const uint32_t**>(registers.rsp + 0x38);
            auto* vmContext = reinterpret_cast<void*>(registers.rsi);
            std::vector<void*> externalObjects;
            std::vector<void*> processedRoots;
            // 先收集全部外部对象，让后续 key/field 路径同样能拒绝越过未解码资源。
            for (uint32_t index = 0; index < instances->count; ++index)
                if (externalInstanceBits[index >> 5] & (uint32_t(1) << (index & 31)))
                    externalObjects.push_back(instances->data[index]);
            for (uint32_t index = 0; index < instances->count; ++index) {
                // 外部 USER 是未解码请求，等它自己的 reader 完成后再处理。
                if (externalInstanceBits[index >> 5] & (uint32_t(1) << (index & 31)))
                    continue;
                auto* root = instances->data[index];
                const auto* rootType = context.GetInstanceType(root);
                if (std::ranges::find(watchedTypes, rootType) == watchedTypes.end() || std::ranges::find(processedRoots, root) != processedRoots.end())
                    continue;
                // 同一解码图中的重复引用只应用一次；不跨资源保存对象地址去重。
                processedRoots.push_back(root);
                try {
                    const auto* uri = get_URI(vmContext, root);
                    if (uri && uri->size) {
                        const auto target = NormalizeUserDataUri(uri->to_str());
                        ApplyDecodedRoot(context, vmContext, root, rootType, target, externalObjects, executionPlan);
                    }
                } catch (const std::exception& error) {
                    _MESSAGE("[UserDataExtension] root=%p processing failed: %s", root, error.what());
                }
            }
        } catch (const std::exception& error) {
            _MESSAGE("[UserDataExtension] decoded graph processing failed: %s", error.what());
        } catch (...) {
            _MESSAGE("[UserDataExtension] decoded graph processing failed: unknown exception");
        }
    });
    _MESSAGE("[UserDataExtension] completed-graph loading hook %s at %p sources=%zu types=%zu", DeserializeRszCompletedGraphHook ? "ready" : "FAILED", DeserializeRszCompletedGraphExit + 9, executionPlan.size(), watchedTypes.size());
    return bool(DeserializeRszCompletedGraphHook);
}
} // namespace UserDataExtension
