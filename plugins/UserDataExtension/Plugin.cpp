#include "UserDataLoading.h"
#include <FileUtils.h>
#include <LogUtils.h>
#include <Windows.h>
#include <ci/Provider.hpp>
#include <exception>
#include <reframework/API.hpp>
#include <utility>

// 向 CI 提供零类型 provider 的登记桥接；本插件不注册托管类型或方法。
CI_REGISTER_PROVIDER(registration) {
    // 配置由 ci_on_post_initialize 统一收集；此处不访问 VM 或 RF SDK。
}

// 在全局 cctor 前关闭配置登记、收集固定目录并安装 USER 解码完成 hook。
// 此阶段只借用 CI Context，不依赖 RF SDK；失败记录日志，不让异常越过 DLL 回调边界。
extern "C" __declspec(dllexport) void ci_on_post_initialize(const ci::ProviderContext* context) noexcept {
    try {
        const auto gameRoot = FileUtils::GetExecutableDirectory();
        auto declarations = UserDataExtension::GetConfigurationRegistry().FinalizeRegistration(gameRoot);
        if (!UserDataExtension::InstallLoadingHook(context->sdk, std::move(declarations))) {
            _MESSAGE("[UserDataExtension] Failed to install USER loading hook");
            return;
        }
        _MESSAGE("[UserDataExtension] pre-cctor registration ready");
    } catch (const std::exception& error) {
        _MESSAGE("[UserDataExtension] startup failed: %s", error.what());
    } catch (...) {
        _MESSAGE("[UserDataExtension] startup failed: unknown exception");
    }
}

// 向 REFramework 声明本 DLL 所需的插件 ABI 版本。
extern "C" __declspec(dllexport) void reframework_plugin_required_version(REFrameworkPluginVersion* version) {
    version->major = REFRAMEWORK_PLUGIN_VERSION_MAJOR;
    version->minor = REFRAMEWORK_PLUGIN_VERSION_MINOR;
    version->patch = REFRAMEWORK_PLUGIN_VERSION_PATCH;
}

// 在 RF SDK 就绪后接入其 API；早期 USER hook 已由 CI-ready 回调安装。
extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* parameter) {
    reframework::API::initialize(parameter);
    return true;
}

// DLL 附加时只初始化日志，不在 loader lock 内访问游戏对象或安装 hook。
BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        initLogger("UserDataExtension");
    return TRUE;
}
