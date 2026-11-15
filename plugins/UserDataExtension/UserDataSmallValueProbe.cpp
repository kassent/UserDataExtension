#include "UserDataSmallValueChecks.h"
#include <GameTypes.h>
#include <lua.hpp>
#include <sol/sol.hpp>

// 显式构建、手动部署的有限实机探针；不随生产 DLL 部署，不安装常驻业务 hook。
namespace UserDataSmallValueProbe {
static const ci::ProviderContext* context{};
static void* requestedUserData{}; // CreateUserData 交出的一份 owned 引用，release_request 归还。

// 仅接受从真实原生调用传入的 ctx；取得线程 ctx 本身不代表已经建立托管作用域。
static void* CheckContext(uintptr_t address) {
    auto* vmctx = reinterpret_cast<void*>(address);
    if (!context || !vmctx || reframework::API::get()->get_vm_context() != vmctx || !RE::read_offset_data<uint32_t>(vmctx, 0x78) || !RE::read_offset_data<uint32_t>(vmctx, 0x7C))
        throw std::runtime_error("probe requires the current native managed call scope");
    return vmctx;
}

// RF Lua 只提供测试入口；调用由一次性 PlayerManager.update 探针投递，结果为无环 JSON 标量快照。
static void InitializeLua(lua_State* state) {
    sol::state_view lua{state};
    auto api = lua.create_named_table("ude_small_value_probe");
    api["run"] = [](uintptr_t address) {
        auto* vmctx = CheckContext(address);
        auto result = UserDataSmallValueTests::Run(*context, vmctx);
        result["vmctx"] = address;
        result["enter_count"] = RE::read_offset_data<uint32_t>(vmctx, 0x78);
        result["call_depth"] = RE::read_offset_data<uint32_t>(vmctx, 0x7C);
        return result.dump();
    };
    api["request"] = [](uintptr_t address) {
        CheckContext(address);
        if (requestedUserData)
            throw std::runtime_error("request already held");
        requestedUserData = context->CreateUserData(context->GetType("app.user_data.AttackAreaParam"), "GameDesign/Enemy/Em0001/00/Data/Em0001_00_Param_AttackAreaParam.user");
        if (!requestedUserData)
            throw std::runtime_error("native USER request failed");
        return uintptr_t(requestedUserData);
    };
    api["snapshot"] = [](uintptr_t address) {
        auto* vmctx = CheckContext(address);
        using namespace UserDataSmallValueTests;
        const std::string prefix = "attackAreaDataList[0].DataPack[0].";
        const auto field = UserDataExtension::ResolveFieldPath(*context, vmctx, requestedUserData, UserDataExtension::ParseFieldPath(prefix + "_RootMove._PosList"));
        const auto list = UserDataExtension::ReadCollection(*context, field.type, *static_cast<void**>(field.address));
        // RootMove 的公开 getter 是实际原生消费者，不只读取 USER 源字段。
        const auto move = UserDataExtension::ResolveFieldPath(*context, vmctx, requestedUserData, UserDataExtension::ParseFieldPath(prefix + "_RootMove"));
        auto* object = *static_cast<void**>(move.address);
        using Fnget_PosList = void* (*)(void*, void*);
        const auto get_PosList = reinterpret_cast<Fnget_PosList>(context->GetMethod(context->GetType("app.RootMoveData"), "get_PosList()"));
        const bool consumerMatches = get_PosList && get_PosList(vmctx, object) == list.list;
        return Json{{"count", list.count}, {"stride", list.stride}, {"first_x", Read<float>(*context, vmctx, requestedUserData, prefix + "_RootMove._PosList[0].x")}, {"last_z", Read<float>(*context, vmctx, requestedUserData, prefix + "_RootMove._PosList[2].z")}, {"offset_y", Read<float>(*context, vmctx, requestedUserData, prefix + "_DataList[0].OffsetLen.y")}, {"shell", Read<int32_t>(*context, vmctx, requestedUserData, prefix + "_DataList[0].ShellIndex._Value")}, {"consumer_matches", consumerMatches}}.dump();
    };
    api["release_request"] = [](uintptr_t address) {
        CheckContext(address);
        if (requestedUserData) {
            context->Release(requestedUserData);
            requestedUserData = nullptr;
        }
    };
}
} // namespace UserDataSmallValueProbe

CI_REGISTER_PROVIDER(registration) {}

// CI-ready 只缓存进程期服务表，不调用 VM 或请求资源。
extern "C" __declspec(dllexport) void ci_on_tdb_ready(const ci::ProviderContext* context) noexcept {
    UserDataSmallValueProbe::context = context;
}

// 仅向 RF Lua 注册有限测试入口，不安装生产调度或修改运行态。
extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* parameter) {
    reframework::API::initialize(parameter);
    parameter->functions->on_lua_state_created(UserDataSmallValueProbe::InitializeLua);
    return true;
}
