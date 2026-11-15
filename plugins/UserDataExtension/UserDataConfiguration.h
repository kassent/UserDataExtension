#pragma once

#include "UserDataFieldPath.h"
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace UserDataExtension {
using Json = nlohmann::json;

enum class OperationKind {
    Append,
    Set,
    Add,
    Subtract,
    Multiply,
    Divide,
    Minimum,
    Maximum,
};
// 将 v1 操作名解析为枚举；未知名字抛异常。
OperationKind ParseOperationKind(std::string_view name);
// 返回有效操作枚举对应的固定日志名称。
const char* GetOperationName(OperationKind kind);

// 已解析的操作声明；路径步骤在登记时缓存，实际字段地址在每次 USER 解码后重新定位。
struct Operation {
    OperationKind kind{};
    Json items;
    Json value;
    std::string key;
    std::vector<FieldPathStep> keySteps;
};

struct FieldOperation {
    std::string fieldPath;
    Operation operation;
    std::vector<FieldPathStep> pathSteps;
};

// 一个同根、字段间独立的批次；rootType 由 CI-ready 绑定，fields 顺序不是依赖执行机制。
struct Patch {
    std::string typeName;
    const ci::Type* rootType{};
    std::vector<FieldOperation> fields;
};

struct Extension {
    std::string target;
    std::vector<Patch> patches;
};

// 已解析的配置快照。文件保留词法规范化的输入 path，不解析虚拟映射背后的物理位置。
// 内存配置仅有 sourceId，不伪造磁盘路径。
// 同一路径由 API/目录登记时合并来源；内存来源单独按区分大小写的 ID 去重。
struct ConfigurationDeclaration {
    std::filesystem::path path;
    std::string sourceId;
    bool fromApi{};
    bool fromDirectory{};
    std::vector<Extension> extensions;
};

// 按目录段再按文件名做稳定序数比较，不使用区域设置或自然数字排序。
bool ConfigurationPathLess(const std::filesystem::path& left, const std::filesystem::path& right);
// 将资源 URI 规范化为匹配用逻辑路径；不代替声明 target 的严格校验。
std::string NormalizeUserDataUri(std::string uri);
// 读取并完整验证一份配置，缓存操作与路径语法；任一错误抛异常，不访问游戏类型。
ConfigurationDeclaration ReadDeclarationFile(const std::filesystem::path& path);
// 返回内存来源 ID 或文件的 UTF-8 路径，供执行器冲突归属与诊断使用。
std::string GetConfigurationSource(const ConfigurationDeclaration& declaration);

class ConfigurationRegistry {
  public:
    // 在窗口内登记绝对路径对应的文件快照；重复路径幂等，失败记日志并返回 false。
    bool RegisterFile(const std::filesystem::path& path) noexcept;
    // 同步解析内存 JSON 并持有快照；同 ID 幂等，失败不发布任何扩展。
    bool RegisterJson(const char* sourceIdUtf8, const char* jsonUtf8) noexcept;
    // 关闭窗口后按来源 ID 排内存配置，再按既有路径顺序排文件；重复调用抛异常。
    std::vector<ConfigurationDeclaration> FinalizeRegistration(const std::filesystem::path& gameRoot);

  private:
    std::mutex mutex;
    bool registrationClosed{};
    std::vector<ConfigurationDeclaration> declarations;
};

// 返回本 DLL 共享的进程期登记表，不额外创建登记入口或缓存层。
ConfigurationRegistry& GetConfigurationRegistry();
} // namespace UserDataExtension
