#pragma once
#include "UserDataConfiguration.h"

namespace UserDataExtension {
// 在 CI-ready 阶段验证计划并安装原生解码完成 hook；无有效配置时无需安装也返回 true。
// 必需服务或定位契约缺失抛异常；成功的计划由回调进程期持有。
bool InstallLoadingHook(const ci::ProviderContext* context, std::vector<ConfigurationDeclaration> declarations);
} // namespace UserDataExtension
