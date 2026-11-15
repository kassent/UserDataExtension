#include "UserDataConfiguration.h"
#include "UserDataExtension.h"
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace UserDataExtension;

// 把登记测试的失败断言转成异常，由入口统一报告并清理自有 fixture。
static void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

// 生成最小有效 v1 声明，作为非法参数变体与文件扫描测试的基线。
static Json MakeDeclaration() {
    return {{"format", "UserDataExtension/v1"}, {"extensions", Json::array({{{"target", "GameDesign/Fixture/Fixture.user"}, {"patches", Json::array({{{"app.FixtureUserData", {{"_Value", {{"op", "set"}, {"value", 10}}}}}}})}}})}};
}

// 把临时文件路径转成 C ABI 所需 UTF-8 文本，覆盖含中文路径的登记。
static std::string PathUtf8(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

// 在进程专属临时目录验证声明语法、重复键、API 幂等/关闭窗口及固定目录加载顺序。
// 随后验证 C ABI 的路径编码与共享登记表；清理仅删除本测试实际创建的文件和空目录。
int main() {
    const auto directory = std::filesystem::temp_directory_path() / ("UserDataRegistrationTests-" + std::to_string(GetCurrentProcessId()));
    std::vector<std::filesystem::path> ownedFiles;
    std::vector<std::filesystem::path> ownedDirectories;
    // 只登记本测试真正创建的目录，清理时不会递归删除预先存在的路径。
    const auto CreateDirectory = [&](const std::filesystem::path& path) {
        if (std::filesystem::create_directory(path))
            ownedDirectories.push_back(path);
    };
    // 写入文本 fixture，并把文件加入本测试的定向清理清单。
    const auto WriteText = [&](const std::filesystem::path& path, const std::string& text) {
        std::ofstream stream(path, std::ios::binary);
        stream << text;
        if (std::ranges::find(ownedFiles, path) == ownedFiles.end())
            ownedFiles.push_back(path);
    };
    // 成功和失败都走同一清理流程；只移除自有文件与逆序排列的空目录。
    const auto RemoveFixtures = [&] {
        // 仅删除本测试实际创建的文件，不递归清理工作区。
        for (const auto& path : ownedFiles)
            std::filesystem::remove(path);
        for (auto directory = ownedDirectories.rbegin(); directory != ownedDirectories.rend(); ++directory)
            std::filesystem::remove(*directory);
    };
    try {
        CreateDirectory(directory);
        const auto declarationPath = directory / "declaration.json";
        WriteText(declarationPath, MakeDeclaration().dump());
        // 大小写不同但可正常读取的输入路径必须原样保留，不解析回磁盘中的实际拼写。
        const auto inputPath = directory / "." / "DECLARATION.JSON";
        Check(ReadDeclarationFile(inputPath).path == inputPath.lexically_normal(),
              "configuration path was resolved instead of lexically normalized");
        // 把 JSON 变体落到同一个文件，确认生产读取器拒绝整份非法声明。
        const auto CheckRejected = [&](const Json& declaration) {
            WriteText(declarationPath, declaration.dump());
            ConfigurationRegistry memoryRegistry;
            Check(!memoryRegistry.RegisterJson("invalid", declaration.dump().c_str()), "invalid in-memory declaration was accepted");
            try {
                ReadDeclarationFile(declarationPath);
            } catch (const std::exception&) {
                return;
            }
            throw std::runtime_error("invalid declaration was accepted");
        };
        // 1. 声明本身：合法解析、操作名/路径缓存、未知参数、非法 target 与重复 JSON 键。
        const auto read = ReadDeclarationFile(declarationPath);
        Check(read.extensions.size() == 1 && read.extensions[0].target == "gamedesign/fixture/fixture.user", "logical target normalization failed");
        Check(read.extensions[0].patches[0].typeName == "app.FixtureUserData" && read.extensions[0].patches[0].fields[0].operation.value == 10, "type-key/field-dictionary parsing failed");
        const auto& field = read.extensions[0].patches[0].fields[0];
        Check(field.operation.kind == OperationKind::Set && field.pathSteps.size() == 1 && field.pathSteps[0].fieldName == "_Value", "operation enum or parsed field path missing");
        for (const auto* name : {"append", "set", "add", "sub", "mul", "div", "min", "max"})
            Check(std::string_view(GetOperationName(ParseOperationKind(name))) == name, "operation name/enum round trip failed");
        auto declaration = MakeDeclaration();
        declaration["unknown"] = true;
        CheckRejected(declaration);
        declaration = MakeDeclaration();
        declaration["format"] = "UserDataExtension/v2";
        CheckRejected(declaration);
        declaration = MakeDeclaration();
        declaration["extensions"] = Json::array();
        CheckRejected(declaration);
        declaration = MakeDeclaration();
        declaration["extensions"][0]["unknown"] = true;
        CheckRejected(declaration);
        for (const auto* target : {"H:/Fixture.user", "/Fixture.user", "../Fixture.user", "GameDesign//Fixture.user", "GameDesign/./Fixture.user", "GameDesign/../Fixture.user", "@GameDesign/Fixture.user", "natives/STM/GameDesign/Fixture.user", "GameDesign/Fixture.user.3", "GameDesign/Fixture.user/", "GameDesign/Fixture.pfb"}) {
            declaration = MakeDeclaration();
            declaration["extensions"][0]["target"] = target;
            CheckRejected(declaration);
        }
        declaration = MakeDeclaration();
        declaration["extensions"][0]["patches"][0]["app.OtherUserData"] = Json::object();
        CheckRejected(declaration);
        declaration = MakeDeclaration();
        declaration["extensions"][0]["patches"][0]["app.FixtureUserData"] = Json::object();
        CheckRejected(declaration);
        for (const auto& invalidOperation : {Json{{"value", 1}}, Json{{"op", "unknown"}, {"value", 1}}, Json{{"op", "set"}}, Json{{"op", "set"}, {"value", 1}, {"items", Json::array({1})}}, Json{{"op", "add"}, {"value", true}}, Json{{"op", "add"}, {"value", 1}, {"key", "_Id"}}, Json{{"op", "append"}, {"items", Json::array()}}, Json{{"op", "append"}, {"items", Json::array({1})}, {"value", 1}}, Json{{"op", "append"}, {"items", Json::array({1})}, {"indexField", ""}}, Json{{"op", "append"}, {"items", Json::array({1})}, {"indexField", "_Index"}}, Json{{"op", "append"}, {"items", Json::array({1})}, {"key", ""}}, Json{{"op", "set"}, {"value", Json{{"app.FixtureRow", {{"$unknown", 1}}}}}}, Json{{"op", "set"}, {"value", Json{{"app.FixtureRow", {{"$uri", "Fixture.user"}, {"_Id", 1}}}}}}, Json{{"op", "set"}, {"value", 1}, {"unrecognized", 1}}}) {
            declaration = MakeDeclaration();
            declaration["extensions"][0]["patches"][0]["app.FixtureUserData"]["_Value"] = invalidOperation;
            CheckRejected(declaration);
        }
        for (const auto* text : {R"({"format":"UserDataExtension/v1","format":"UserDataExtension/v1","extensions":[]})", R"({"format":"UserDataExtension/v1","extensions":[{"target":"Fixture.user","patches":[{"app.FixtureUserData":{"_Value":{"op":"set","value":{"app.FixtureRow":{"_Id":1,"_Id":2}}}}}]}]})", R"({"format":"UserDataExtension/v1","extensions":[{"target":"Fixture.user","patches":[{"app.FixtureUserData":{"_Value":{"op":"set","value":1},"_Value":{"op":"set","value":2}}}]}]})"}) {
            ConfigurationRegistry memoryRegistry;
            Check(!memoryRegistry.RegisterJson("duplicate-keys", text), "in-memory duplicate JSON key was accepted");
            WriteText(declarationPath, text);
            bool rejected = false;
            try {
                ReadDeclarationFile(declarationPath);
            } catch (const std::exception&) {
                rejected = true;
            }
            Check(rejected, "duplicate JSON key was accepted");
        }
        declaration = MakeDeclaration();
        declaration["extensions"][0]["patches"][0]["app.FixtureUserData"]["_Reference"] = {{"op", "set"}, {"value", {{"app.FixtureUserData", {{"$uri", "GameDesign/Fixture.user"}}}}}};
        declaration["extensions"][0]["patches"][0]["app.FixtureUserData"]["_Items"] = {{"op", "append"}, {"key", "_Id"}, {"items", Json::array({{{"app.FixtureRow", {{"_Id", 1}, {"_Index", "$index"}, {"_Nested", {{"app.FixtureValue", {{"_Value", 2}}}}}}}}})}};
        WriteText(declarationPath, declaration.dump());
        Check(ReadDeclarationFile(declarationPath).extensions[0].patches[0].fields.size() == 3, "typed objects and resource metadata were rejected");

        const auto gameRoot = directory / "Game";
        CreateDirectory(gameRoot);
        CreateDirectory(gameRoot / "reframework");
        CreateDirectory(gameRoot / "reframework/data");
        const auto autoRoot = gameRoot / "reframework/data/UserDataExtension";
        CreateDirectory(autoRoot);
        for (const auto* name : {"A-Extra", "A", "b", "00", "2", "10"})
            CreateDirectory(autoRoot / name);
        CreateDirectory(autoRoot / "A/nested");
        WriteText(autoRoot / "A-Extra/00.json", MakeDeclaration().dump());
        WriteText(autoRoot / "A/20.json", MakeDeclaration().dump());
        WriteText(autoRoot / "A/00.JSON", MakeDeclaration().dump());
        WriteText(autoRoot / "A/10.json", MakeDeclaration().dump());
        WriteText(autoRoot / "A/nested/ignored.json", MakeDeclaration().dump());
        WriteText(autoRoot / "ignored.json", MakeDeclaration().dump());
        WriteText(autoRoot / "b/00.json", MakeDeclaration().dump());
        WriteText(autoRoot / "00/10.json", MakeDeclaration().dump());
        WriteText(autoRoot / "00/00.json", MakeDeclaration().dump());
        WriteText(autoRoot / "2/00.json", MakeDeclaration().dump());
        WriteText(autoRoot / "10/00.json", MakeDeclaration().dump());
        WriteText(autoRoot / "A/broken.json", "{invalid");
        WriteText(autoRoot / "A/ignored.txt", MakeDeclaration().dump());
        WriteText(autoRoot / "A/README", "not a configuration file");
        // 2. 文件收集：目录分组顺序、扫描深度、坏文件隔离及 API/目录同路径快照去重。
        ConfigurationRegistry registry;
        const auto apiPath = autoRoot / "A/10.json";
        Check(registry.RegisterFile(apiPath), "API fixture registration failed");
        WriteText(apiPath, "{changed after successful registration");
        Check(registry.RegisterFile(apiPath.parent_path() / ".." / "A" / "10.json"), "idempotent registration reread a changed file");
        Check(registry.RegisterFile(declarationPath), "outside-directory API registration failed");
        const auto files = registry.FinalizeRegistration(gameRoot);
        const std::vector<std::string> expected{"00/00.json", "00/10.json", "10/00.json", "2/00.json", "A/00.JSON", "A/10.json", "A/20.json", "A-Extra/00.json", "b/00.json"};
        std::vector<std::string> actual;
        for (const auto& file : files) {
            if (file.fromDirectory)
                actual.push_back(PathUtf8(file.path.lexically_relative(autoRoot)));
        }
        Check(actual == expected, "folder/file ordinal order, scan depth, or bad-file continuation failed");
        Check(files.size() == expected.size() + 1 && std::ranges::is_sorted(files,
                                                                            [](const ConfigurationDeclaration& left, const ConfigurationDeclaration& right) {
                                                                                return ConfigurationPathLess(left.path, right.path);
                                                                            }),
              "API and directory files did not share one path order");
        const auto mixed = std::ranges::find_if(files, [&](const ConfigurationDeclaration& file) {
            return file.path == apiPath;
        });
        Check(mixed != files.end() && mixed->fromApi && mixed->fromDirectory && mixed->extensions[0].patches[0].fields[0].operation.value == 10, "mixed-source dedupe lost the first successful content");
        Check(!registry.RegisterFile(declarationPath), "late API registration was accepted");
        ConfigurationRegistry missingDirectoryRegistry;
        Check(missingDirectoryRegistry.FinalizeRegistration(directory / "MissingGame").empty(), "missing fixed directory was treated as a fatal error");
        ConfigurationRegistry relativeRegistry;
        Check(!relativeRegistry.RegisterFile("relative.json"), "relative API path was accepted");
        ConfigurationRegistry rejectedFileRegistry;
        declaration = MakeDeclaration();
        declaration["extensions"].push_back({{"target", "Fixture.user"}, {"patches", Json::array({{{"app.FixtureUserData", {{"_Value", {{"op", "unknown"}, {"value", 1}}}}}}})}});
        WriteText(declarationPath, declaration.dump());
        Check(!rejectedFileRegistry.RegisterFile(declarationPath), "partially valid configuration was accepted");
        Check(!rejectedFileRegistry.RegisterJson("partial", declaration.dump().c_str()), "partially valid in-memory configuration was accepted");
        Check(rejectedFileRegistry.FinalizeRegistration(directory / "MissingGame").empty(), "failed registration published part of a file");
        WriteText(declarationPath, MakeDeclaration().dump());
        const auto unicodePath = directory / L"\u6269\u5C55\u914D\u7F6E.json";
        WriteText(unicodePath, MakeDeclaration().dump());
        // 3. 内存配置：同步持有、ID 幂等、失败不占 ID、UTF-8、排序及与文件身份域隔离。
        ConfigurationRegistry memoryRegistry;
        const auto jsonText = MakeDeclaration().dump();
        Check(!memoryRegistry.RegisterJson(nullptr, jsonText.c_str()) && !memoryRegistry.RegisterJson("", jsonText.c_str()) && !memoryRegistry.RegisterJson("bad", nullptr) && !memoryRegistry.RegisterJson("bad", "") && !memoryRegistry.RegisterJson("\xC3\x28", jsonText.c_str()), "invalid in-memory inputs were accepted");
        Check(!memoryRegistry.RegisterJson("B", "{broken"), "invalid JSON registered successfully");
        std::string sourceId = "B", transientJson = jsonText;
        Check(memoryRegistry.RegisterJson(sourceId.c_str(), transientJson.c_str()), "failed registration occupied its source ID");
        sourceId.assign("destroyed");
        transientJson.assign("destroyed");
        Check(memoryRegistry.RegisterJson("B", "{changed after registration"), "duplicate source did not keep the first successful snapshot");
        Check(memoryRegistry.RegisterJson("A", jsonText.c_str()) && memoryRegistry.RegisterJson("a", jsonText.c_str()) && memoryRegistry.RegisterJson("扩展/身份", jsonText.c_str()), "valid memory sources were rejected");
        Check(memoryRegistry.RegisterFile(declarationPath) && memoryRegistry.RegisterJson(PathUtf8(declarationPath).c_str(), jsonText.c_str()), "file path and memory ID identities collided");
        const auto memorySources = memoryRegistry.FinalizeRegistration(directory / "MissingGame");
        Check(memorySources.size() == 6 && memorySources[0].sourceId == "A" && memorySources[1].sourceId == "B" && memorySources[3].sourceId == "a" && memorySources.back().path == declarationPath, "memory source ID or memory-before-file order failed");
        const auto& snapshot = memorySources[1];
        Check(snapshot.path.empty() && snapshot.sourceId == "B" && snapshot.extensions[0].patches[0].fields[0].operation.value == 10, "registration borrowed caller buffers or synthesized a file path");
        Check(GetConfigurationSource(snapshot) == "B" && !memoryRegistry.RegisterJson("B", jsonText.c_str()), "source identity or closed memory registration window failed");
        // 4. 两种 C ABI 共用登记表，内容接口不借用缓冲，也不访问来源 ID 对应的路径。
        Check(!RegisterUserDataExtension(nullptr) && !RegisterUserDataExtension("") && !RegisterUserDataExtension("relative.json") && !RegisterUserDataExtension("\xC3\x28"), "C ABI accepted invalid path input");
        Check(RegisterUserDataExtension(PathUtf8(declarationPath).c_str()), "C ABI valid UTF-8 registration failed");
        Check(RegisterUserDataExtension(PathUtf8(unicodePath).c_str()), "C ABI Unicode UTF-8 registration failed");
        Check(!RegisterUserDataExtensionJson(nullptr, jsonText.c_str()) && !RegisterUserDataExtensionJson("ABI", nullptr) && !RegisterUserDataExtensionJson("ABI", "{broken"), "JSON C ABI accepted invalid input");
        Check(RegisterUserDataExtensionJson("ABI", jsonText.c_str()), "JSON C ABI registration failed");
        const auto apiFiles = GetConfigurationRegistry().FinalizeRegistration(directory / "MissingGame");
        Check(apiFiles.size() == 3 && apiFiles[0].sourceId == "ABI" && apiFiles[0].path.empty() && std::ranges::all_of(apiFiles, [](const ConfigurationDeclaration& file) {
                  return file.fromApi;
              }),
              "C ABI did not use the shared registration registry");
        Check(!RegisterUserDataExtensionJson("late", jsonText.c_str()), "JSON C ABI accepted registration after finalization");
        RemoveFixtures();
        std::cout << "UserData registration tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        RemoveFixtures();
        return 1;
    }
}
