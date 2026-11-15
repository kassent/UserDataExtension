#pragma once

#if defined(USER_DATA_EXTENSION_EXPORTS)
#define USER_DATA_EXTENSION_API __declspec(dllexport)
#elif defined(USER_DATA_EXTENSION_STATIC)
#define USER_DATA_EXTENSION_API
#else
#define USER_DATA_EXTENSION_API __declspec(dllimport)
#endif

// 在 CI 登记窗口传入绝对 UTF-8 配置路径；成功只表示读取并登记，不表示 USER 已加载。
// 路径或配置错误返回 false 并记日志；调用方无需交出路径内存所有权。
extern "C" USER_DATA_EXTENSION_API bool __cdecl RegisterUserDataExtension(const char* jsonPathUtf8) noexcept;

// 在同一登记窗口同步解析 NUL 结尾的 UTF-8 JSON；返回后调用方可释放两个字符串。
// 来源 ID 区分大小写，同 ID 保留首次成功快照；内存来源按 ID 排序，先于文件来源执行。
extern "C" USER_DATA_EXTENSION_API bool __cdecl RegisterUserDataExtensionJson(const char* sourceIdUtf8, const char* jsonUtf8) noexcept;
