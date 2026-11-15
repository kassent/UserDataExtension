#include "UserDataConfiguration.h"
#include "UserDataExtension.h"
#include <LogUtils.h>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace UserDataExtension {
// JSON DTO 只负责字段映射；操作语义和互斥参数由 ReadOperation 显式校验。
struct OperationJson {
    std::string op;
    Json items;
    Json value;
    std::string key;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(OperationJson, op, items, value, key)

static constexpr std::array operationNames{"append", "set", "add", "sub", "mul", "div", "min", "max"};

// 把 JSON 操作名转换为执行枚举；未知名字在登记阶段报错，加载时不再解析字符串。
OperationKind ParseOperationKind(std::string_view name) {
    const auto found = std::ranges::find(operationNames, name);
    if (found == operationNames.end())
        throw std::runtime_error("unknown operation: " + std::string(name));
    return static_cast<OperationKind>(found - operationNames.begin());
}

// 把已解析的操作枚举还原为固定名称，供错误和应用日志使用。
const char* GetOperationName(OperationKind kind) {
    return operationNames.at(std::to_underlying(kind));
}

struct ExtensionJson {
    std::string target;
    Json patches;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ExtensionJson, target, patches)

struct DeclarationJson {
    std::string format;
    std::vector<ExtensionJson> extensions;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DeclarationJson, format, extensions)

// 把 Windows 文件路径转成使用正斜杠的 UTF-8 文本，仅用于日志。
static std::string PathUtf8(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

// 使用 Windows 序数规则比较两个路径片段，返回负数、零或正数。
// 可忽略大小写，但不采用用户区域设置或自然数字排序；长度/系统错误显式报错。
static int CompareOrdinal(const std::wstring& left, const std::wstring& right, bool ignoreCase) {
    if (left.size() > INT_MAX || right.size() > INT_MAX)
        throw std::runtime_error("configuration path segment exceeds ordinal comparison range");
    const auto result = CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), ignoreCase);
    if (!result)
        throw std::runtime_error("configuration path ordinal comparison failed");
    return result - CSTR_EQUAL;
}

// 按目录段逐段比较，最后比较文件名，使 API 文件与固定目录文件共用稳定顺序。
// 每段先忽略大小写比较，相同时再按原字形打破平局；例如 10 排在 2 前。
bool ConfigurationPathLess(const std::filesystem::path& left, const std::filesystem::path& right) {
    auto leftSegment = left.begin();
    auto rightSegment = right.begin();
    for (; leftSegment != left.end() && rightSegment != right.end(); ++leftSegment, ++rightSegment) {
        const auto caseInsensitive = CompareOrdinal(leftSegment->native(), rightSegment->native(), true);
        if (caseInsensitive)
            return caseInsensitive < 0;
        const auto original = CompareOrdinal(leftSegment->native(), rightSegment->native(), false);
        if (original)
            return original < 0;
    }
    return leftSegment == left.end() && rightSegment != right.end();
}

// 按 Windows 不区分大小写的路径语义判定两份配置是否使用同一路径。
static bool SameConfigurationPath(const std::filesystem::path& left, const std::filesystem::path& right) {
    return CompareOrdinal(left.native(), right.native(), true) == 0;
}

// 要求绝对、无 NUL 的配置路径，仅整理路径段；读取、排序和去重共用此路径。
// 不解析 MO2 虚拟路径背后的物理位置，文件重映射及覆盖交给文件系统层。
static std::filesystem::path NormalizeConfigurationPath(const std::filesystem::path& path) {
    if (!path.is_absolute() || path.native().find(L'\0') != std::wstring::npos)
        throw std::runtime_error("configuration path must be an absolute path without NUL");
    return path.lexically_normal();
}

// 统一 USER URI 的分隔符、ASCII 大小写及常见资源包装前后缀，得到匹配用逻辑路径。
// 这里只规范化；配置 target 是否符合格式由 ValidateTarget 单独检查。
std::string NormalizeUserDataUri(std::string uri) {
    for (auto& character : uri) {
        if (character == '\\')
            character = '/';
        else if (character >= 'A' && character <= 'Z')
            character += 'a' - 'A';
    }
    if (uri.starts_with('@'))
        uri.erase(0, 1);
    if (uri.starts_with("natives/stm/"))
        uri.erase(0, 12);
    if (uri.ends_with(".3"))
        uri.resize(uri.size() - 2);
    return uri;
}

// 检查配置控制对象只包含允许的键；游戏对象的字段字典不走这份白名单。
static void ValidateControlKeys(const Json& value, std::initializer_list<const char*> keys, const char* objectName) {
    if (!value.is_object())
        throw std::runtime_error(std::string(objectName) + " must be an object");
    for (const auto& [key, member] : value.items()) {
        if (std::ranges::none_of(keys, [&](const char* allowed) {
                return key == allowed;
            }))
            throw std::runtime_error(std::string(objectName) + " has unknown control key: " + key);
    }
}

// 校验 target 是无包装层、无绝对路径或目录穿越的逻辑 .user 路径。
// 不把错误声明悄悄修正为另一个目标，避免配置应用到意外资源。
static void ValidateTarget(const std::string& target) {
    const auto normalized = NormalizeUserDataUri(target);
    if (target.empty() || target.find('\0') != target.npos || target.find(':') != target.npos || normalized.starts_with('/') || target.starts_with('@') || !normalized.ends_with(".user"))
        throw std::runtime_error("target must be an engine-relative USER path");
    std::string lowercase = target;
    for (auto& character : lowercase) {
        if (character == '\\')
            character = '/';
        else if (character >= 'A' && character <= 'Z')
            character += 'a' - 'A';
    }
    if (lowercase.starts_with("natives/") || lowercase.ends_with(".3"))
        throw std::runtime_error("target must omit natives/STM and file version suffix");
    size_t begin = 0;
    while (begin <= normalized.size()) {
        const auto end = normalized.find('/', begin);
        const auto segment = normalized.substr(begin, end == normalized.npos ? end : end - begin);
        if (segment.empty() || segment == "." || segment == "..")
            throw std::runtime_error("target contains an empty or relative path segment");
        if (end == normalized.npos)
            break;
        begin = end + 1;
    }
}

// 只按 JSON 结构递归检查类型键、字段字典及 $uri 声明，不读取 TDB 或创建对象。
// 允许标量、数组和已约定的 bytes 表示；具体字段类型留给 CI-ready 阶段验证。
static void ValidateObjectValue(const Json& value) {
    if (value.is_array()) {
        for (const auto& element : value)
            ValidateObjectValue(element);
        return;
    }
    if (!value.is_object())
        return;
    // 既有 via.vec3 的已验证编码；具体目标类型由 TDB 绑定阶段限制。
    if (value.size() == 1 && value.contains("bytes") && value.at("bytes").is_string())
        return;
    if (value.size() != 1 || value.begin().key().empty() || value.begin().key().find('\0') != std::string::npos || value.begin().key().starts_with('$') || !value.begin().value().is_object())
        throw std::runtime_error("object value must contain exactly one type key and a field dictionary");
    const auto& fields = value.begin().value();
    if (fields.contains("$uri")) {
        if (fields.size() != 1 || !fields.at("$uri").is_string() || fields.at("$uri").get_ref<const std::string&>().empty() || fields.at("$uri").get_ref<const std::string&>().find('\0') != std::string::npos)
            throw std::runtime_error("$uri must be a nonempty string without field initialization");
        return;
    }
    for (const auto& [name, member] : fields.items()) {
        if (name.empty() || name.find('\0') != name.npos || name.starts_with('$'))
            throw std::runtime_error("object has an empty field or unknown metadata key: " + name);
        ValidateObjectValue(member);
    }
}

// 读取一条操作，区分 append 的 items/key 与 set/算术的 value 参数。
// 先验证声明形状，再缓存操作枚举及 key 路径步骤，避免加载时重复解析。
static Operation ReadOperation(const Json& value) {
    ValidateControlKeys(value, {"op", "items", "value", "key"}, "operation");
    if (!value.contains("op") || !value.at("op").is_string())
        throw std::runtime_error("operation requires a string op");
    auto description = value.get<OperationJson>();
    Operation operation{.kind = ParseOperationKind(description.op), .items = std::move(description.items), .value = std::move(description.value), .key = std::move(description.key)};
    if (operation.kind == OperationKind::Append) {
        if (!value.contains("items") || !operation.items.is_array() || operation.items.empty() || value.contains("value"))
            throw std::runtime_error("append requires nonempty items and does not accept value");
        if (value.contains("key") && (operation.key.empty() || operation.key.find('\0') != operation.key.npos))
            throw std::runtime_error("append key must be nonempty when present");
        for (const auto& item : operation.items)
            ValidateObjectValue(item);
    } else {
        if (!value.contains("value") || value.contains("items") || value.contains("key"))
            throw std::runtime_error("set/arithmetic requires value and does not accept append parameters");
        if (operation.kind == OperationKind::Set)
            ValidateObjectValue(operation.value);
        else if (!operation.value.is_number() && !operation.value.is_string())
            throw std::runtime_error("arithmetic value must be a number or decimal integer string");
    }
    if (!operation.key.empty() && operation.key != "$uri")
        operation.keySteps = ParseFieldPath(operation.key);
    return operation;
}

// 把文件或内存 JSON 解析为有序扩展声明；任一语法或结构错误使整份登记失败。
// 解析回调先拒绝重复 JSON 键，再校验格式/目标/类型键/操作字典并缓存字段路径。
// 此阶段不访问游戏元数据、USER 资源或托管对象。
static ConfigurationDeclaration ParseDeclarationJson(std::string_view text) {
    std::vector<std::set<std::string>> objectKeys;
    // 为每层对象记录已出现的键，避免 JSON 库的后值覆盖掩盖重复字段声明。
    const auto json = Json::parse(text, [&](int, Json::parse_event_t event, Json& parsed) {
        if (event == Json::parse_event_t::object_start)
            objectKeys.emplace_back();
        else if (event == Json::parse_event_t::key && !objectKeys.back().insert(parsed.get<std::string>()).second)
            throw std::runtime_error("duplicate JSON key: " + parsed.get<std::string>());
        else if (event == Json::parse_event_t::object_end)
            objectKeys.pop_back();
        return true;
    });
    ValidateControlKeys(json, {"format", "extensions"}, "configuration");
    const auto declaration = json.get<DeclarationJson>();
    if (declaration.format != "UserDataExtension/v1" || declaration.extensions.empty())
        throw std::runtime_error("configuration requires UserDataExtension/v1 and nonempty extensions");
    ConfigurationDeclaration file;
    for (size_t index = 0; index < declaration.extensions.size(); ++index) {
        ValidateControlKeys(json.at("extensions").at(index), {"target", "patches"}, "extension");
        const auto& extensionJson = declaration.extensions[index];
        ValidateTarget(extensionJson.target);
        if (!extensionJson.patches.is_array() || extensionJson.patches.empty())
            throw std::runtime_error("extension requires nonempty patches");
        Extension extension{.target = NormalizeUserDataUri(extensionJson.target)};
        for (const auto& patchJson : extensionJson.patches) {
            if (!patchJson.is_object() || patchJson.size() != 1 || patchJson.begin().key().empty() || patchJson.begin().key().find('\0') != std::string::npos || !patchJson.begin().value().is_object() || patchJson.begin().value().empty())
                throw std::runtime_error("patch requires one root type key and a nonempty operation dictionary");
            Patch patch{.typeName = patchJson.begin().key()};
            for (const auto& [field, operationJson] : patchJson.begin().value().items()) {
                if (field.empty() || field.find('\0') != field.npos)
                    throw std::runtime_error("operation field path must be nonempty without NUL");
                patch.fields.push_back({field, ReadOperation(operationJson), ParseFieldPath(field)});
            }
            extension.patches.push_back(std::move(patch));
        }
        file.extensions.push_back(std::move(extension));
    }
    return file;
}

// 文件入口只负责路径与读取；JSON 结构校验与内存入口共用同一解析器。
ConfigurationDeclaration ReadDeclarationFile(const std::filesystem::path& path) {
    const auto normalizedPath = NormalizeConfigurationPath(path);
    std::ifstream stream(normalizedPath, std::ios::binary);
    if (!stream)
        throw std::runtime_error("configuration file could not be opened");
    const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    auto declaration = ParseDeclarationJson(text);
    declaration.path = normalizedPath;
    return declaration;
}

// 来源用于日志及写入冲突归属；内存 ID 与真实文件路径保持各自身份域。
std::string GetConfigurationSource(const ConfigurationDeclaration& declaration) {
    return declaration.sourceId.empty() ? PathUtf8(declaration.path) : declaration.sourceId;
}

// 在登记窗口内读取并保存 API 提供的配置快照；同一规范化路径重复登记保持幂等。
// 锁保护登记状态与快照；已关闭、路径或文件错误记录日志并返回 false，不跨 ABI 抛异常。
bool ConfigurationRegistry::RegisterFile(const std::filesystem::path& path) noexcept {
    try {
        const auto normalizedPath = NormalizeConfigurationPath(path);
        std::lock_guard lock(mutex);
        if (registrationClosed)
            throw std::runtime_error("configuration registration window has closed");
        const auto found = std::ranges::find_if(declarations, [&](const ConfigurationDeclaration& file) {
            return !file.path.empty() && SameConfigurationPath(file.path, normalizedPath);
        });
        if (found != declarations.end()) {
            found->fromApi = true;
            return true;
        }
        auto file = ReadDeclarationFile(normalizedPath);
        file.fromApi = true;
        declarations.push_back(std::move(file));
        _MESSAGE("[UserDataExtension] Registered source=API path=%s", PathUtf8(normalizedPath).c_str());
        return true;
    } catch (const std::exception& error) {
        _MESSAGE("[UserDataExtension] Failed registration path=%s reason=%s", PathUtf8(path).c_str(), error.what());
    } catch (...) {
        _MESSAGE("[UserDataExtension] Failed registration: unknown exception");
    }
    return false;
}

// 在窗口内同步解析并保存内存快照；来源 ID 不作为文件路径规范化或访问磁盘。
// 与文件登记一致，重复身份保留首次成功内容，解析失败不占用该 ID。
bool ConfigurationRegistry::RegisterJson(const char* sourceIdUtf8, const char* jsonUtf8) noexcept {
    try {
        if (!sourceIdUtf8 || !*sourceIdUtf8 || !jsonUtf8 || !*jsonUtf8)
            throw std::runtime_error("source ID and JSON must be nonempty UTF-8 strings");
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, sourceIdUtf8, -1, nullptr, 0))
            throw std::runtime_error("source ID is not valid UTF-8");
        std::lock_guard lock(mutex);
        if (registrationClosed)
            throw std::runtime_error("configuration registration window has closed");
        if (std::ranges::any_of(declarations, [&](const ConfigurationDeclaration& declaration) {
                return declaration.sourceId == sourceIdUtf8;
            }))
            return true;
        auto declaration = ParseDeclarationJson(jsonUtf8);
        declaration.sourceId = sourceIdUtf8;
        declaration.fromApi = true;
        declarations.push_back(std::move(declaration));
        _MESSAGE("[UserDataExtension] Registered source=JSON id=%s", sourceIdUtf8);
        return true;
    } catch (const std::exception& error) {
        _MESSAGE("[UserDataExtension] Failed JSON registration id=%s reason=%s", sourceIdUtf8 ? sourceIdUtf8 : "<null>", error.what());
    } catch (...) {
        _MESSAGE("[UserDataExtension] Failed JSON registration: unknown exception");
    }
    return false;
}

// 只扫描固定目录下一级分组目录中的直接子 JSON，不读取根目录文件或更深子目录。
// 坏目录/条目记日志并继续，最终按目录段和文件名的序数规则排序。
static std::vector<std::filesystem::path> CollectConfigurationFiles(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    if (!std::filesystem::exists(directory, error)) {
        if (error)
            _MESSAGE("[UserDataExtension] Failed directory path=%s reason=%s", PathUtf8(directory).c_str(), error.message().c_str());
        return files;
    }
    std::filesystem::directory_iterator groups(directory, error);
    while (!error && groups != std::filesystem::directory_iterator{}) {
        const auto group = *groups;
        std::error_code groupError;
        if (group.is_directory(groupError)) {
            std::filesystem::directory_iterator entries(group.path(), groupError);
            while (!groupError && entries != std::filesystem::directory_iterator{}) {
                const auto entry = *entries;
                std::error_code entryError;
                if (entry.is_regular_file(entryError) && CompareOrdinal(entry.path().extension().native(), L".json", true) == 0)
                    files.push_back(entry.path());
                if (entryError)
                    _MESSAGE("[UserDataExtension] Failed entry path=%s reason=%s", PathUtf8(entry.path()).c_str(), entryError.message().c_str());
                entries.increment(groupError);
            }
        }
        if (groupError)
            _MESSAGE("[UserDataExtension] Failed group path=%s reason=%s", PathUtf8(group.path()).c_str(), groupError.message().c_str());
        groups.increment(error);
    }
    if (error)
        _MESSAGE("[UserDataExtension] Failed directory path=%s reason=%s", PathUtf8(directory).c_str(), error.message().c_str());
    std::ranges::sort(files, ConfigurationPathLess);
    return files;
}

// 一次性关闭 API 登记窗口并取走快照，再合并固定目录扫描结果，发布统一排序的计划。
// 重复路径保留首次成功读取的内容，只合并来源标记；坏文件不影响其他配置继续收集。
// 锁只包住窗口关闭与快照交接，不让磁盘扫描长期占用登记锁。
std::vector<ConfigurationDeclaration> ConfigurationRegistry::FinalizeRegistration(const std::filesystem::path& gameRoot) {
    std::vector<ConfigurationDeclaration> collected;
    {
        std::lock_guard lock(mutex);
        if (registrationClosed)
            throw std::runtime_error("configuration registry was already prepared");
        registrationClosed = true;
        collected = std::move(declarations);
    }
    // API 已接收的内容是快照：固定目录再次发现同一路径时只补来源，不重新读取。
    for (const auto& path : CollectConfigurationFiles(gameRoot / "reframework/data/UserDataExtension")) {
        try {
            const auto normalizedPath = NormalizeConfigurationPath(path);
            const auto found = std::ranges::find_if(collected, [&](const ConfigurationDeclaration& file) {
                return !file.path.empty() && SameConfigurationPath(file.path, normalizedPath);
            });
            if (found != collected.end()) {
                found->fromDirectory = true;
                continue;
            }
            auto file = ReadDeclarationFile(normalizedPath);
            file.fromDirectory = true;
            collected.push_back(std::move(file));
            _MESSAGE("[UserDataExtension] Registered source=Directory path=%s", PathUtf8(normalizedPath).c_str());
        } catch (const std::exception& error) {
            _MESSAGE("[UserDataExtension] Failed automatic registration path=%s reason=%s", PathUtf8(path).c_str(), error.what());
        }
    }
    // 内存声明先按 UTF-8 ID 的区分大小写字节序执行；文件间原有排序不变。
    std::ranges::sort(collected, [](const ConfigurationDeclaration& left, const ConfigurationDeclaration& right) {
        if (left.path.empty() != right.path.empty())
            return left.path.empty();
        if (left.path.empty())
            return left.sourceId < right.sourceId;
        return ConfigurationPathLess(left.path, right.path);
    });
    for (size_t index = 0; index < collected.size(); ++index) {
        const auto& file = collected[index];
        _MESSAGE("[UserDataExtension] Order=%zu source=%s%s name=%s", index, !file.sourceId.empty() ? "JSON" : (file.fromApi ? "API" : ""), file.fromDirectory ? (file.fromApi ? "+Directory" : "Directory") : "", GetConfigurationSource(file).c_str());
    }
    return collected;
}

// 返回本 DLL 进程期唯一的配置登记表，供 C 导出与 CI-ready 启动流程共享。
ConfigurationRegistry& GetConfigurationRegistry() {
    static ConfigurationRegistry registry;
    return registry;
}
} // namespace UserDataExtension

// C ABI 登记入口：严格解码绝对 UTF-8 路径，再交给共享登记表读取配置。
// 返回 true 仅表示登记成功，不表示 USER 已加载或扩展已生效；所有异常转换为日志和 false。
extern "C" bool __cdecl RegisterUserDataExtension(const char* jsonPathUtf8) noexcept {
    try {
        if (!jsonPathUtf8 || !*jsonPathUtf8)
            throw std::runtime_error("configuration path must be nonempty UTF-8");
        const auto length = strlen(jsonPathUtf8);
        if (length > INT_MAX)
            throw std::runtime_error("configuration UTF-8 path exceeds Windows range");
        const auto wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, jsonPathUtf8, static_cast<int>(length), nullptr, 0);
        if (!wideLength)
            throw std::runtime_error("configuration path is not valid UTF-8");
        std::wstring widePath(wideLength, L'\0');
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, jsonPathUtf8, static_cast<int>(length), widePath.data(), wideLength))
            throw std::runtime_error("configuration UTF-8 path conversion failed");
        return UserDataExtension::GetConfigurationRegistry().RegisterFile(std::filesystem::path(widePath));
    } catch (const std::exception& error) {
        _MESSAGE("[UserDataExtension] Failed API registration: %s", error.what());
    } catch (...) {
        _MESSAGE("[UserDataExtension] Failed API registration: unknown exception");
    }
    return false;
}

// C ABI 内容入口与路径入口共享登记窗口；内部捕获异常，调用方只借出同步输入字符串。
extern "C" bool __cdecl RegisterUserDataExtensionJson(const char* sourceIdUtf8, const char* jsonUtf8) noexcept {
    return UserDataExtension::GetConfigurationRegistry().RegisterJson(sourceIdUtf8, jsonUtf8);
}
