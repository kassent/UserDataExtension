#pragma once
#include <array>
#include <ci/API.hpp>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <vector>

namespace UserDataExtension {
// 返回已支持标量/枚举/Guid 的实际字节数；不猜测复杂结构体数组布局，未知类型抛异常。
uint32_t GetScalarSize(const ci::SDKAPI& context, ci::TypeDefinitionHandle type);
// 首批支持的纯值结构；成员按有效值比较，不把对齐 padding 当成身份。
struct SupportedValueLayout {
    ci::TypeDefinitionHandle type{};
    uint32_t size{};
    std::array<ci::API::Field, 4> members{};
    uint32_t memberCount{};
    bool nullable{}; // _HasValue=false 时身份不读取未生效的 _Value。
};
// 非首批结构返回 nullptr；命中结构必须具备已核实的纯标量成员和 <=16 字节布局。
const SupportedValueLayout* FindSupportedValueLayout(const ci::SDKAPI& context, ci::TypeDefinitionHandle type);
// 集合及整值写入共用；原标量走旧规则，新增纯值结构取 CI 的真实大小。
uint32_t GetSupportedValueSize(const ci::SDKAPI& context, ci::TypeDefinitionHandle type);
// 判断整数/枚举底层 payload 是否无符号，供精确赋值及有界算术选择转换分支。
bool IsUnsignedInteger(const ci::SDKAPI& context, ci::TypeDefinitionHandle type);
// 递归校验 JSON 与元数据布局，不请求资源或创建托管对象；失败抛异常。
// elementIndex 有值表示处于集合元素内；校验只需占位下标，内层集合自行建立新上下文。
void ValidateJsonValue(const ci::SDKAPI& context, ci::TypeDefinitionHandle type, uint32_t valueSize, const nlohmann::json& value, std::optional<int32_t> elementIndex = std::nullopt);

class ObjectBuilder {
  public:
    // 借用当前原生调用的 Context/vmctx；不自行建立托管作用域或分配对象。
    ObjectBuilder(const ci::SDKAPI& context, void* vmContext)
        : context{context}, vmContext{vmContext} {}
    // 归还本构造器持有的外部 USER 请求引用，普通新建托管对象不在这里逐一释放。
    ~ObjectBuilder();
    // 禁用复制，避免两份构造器重复归还同一组 owned USER 请求。
    ObjectBuilder(const ObjectBuilder&) = delete;
    // 禁用复制赋值，保持 owned 请求只有一个归还者。
    ObjectBuilder& operator=(const ObjectBuilder&) = delete;
    // 按期望引用类型创建并填充对象；普通对象不额外 retain，调用方须在当前作用域内持有/发布。
    // $uri 返回的是本构造器持有的资源请求，不保证内容已解码。
    // elementIndex 沿普通对象传递，供 Int32 字段的 "$index" 使用；内层数组/List 从零重新计数。
    void* CreateObject(ci::TypeDefinitionHandle expectedType, const nlohmann::json& value, std::optional<int32_t> elementIndex = std::nullopt);
    // 借用本构造器已请求的外部 USER 列表，供执行器阻止穿越未解码字段；不转移引用。
    std::span<void* const> GetUserDataRequests() const {
        return ownedUserDataRequests;
    }
    // 把 JSON 转换并写入指定字段或暂存值；普通引用字段必须提供真实 owner 以执行写屏障。
    // owner=null 仅适用于尚未发布的数组/准备缓冲；非法类型、范围或布局抛异常。
    // "$index" 仅在 Int32 且 elementIndex 有值时替换；字符串中的同名文本保持原样。
    void WriteValue(void* owner, void* destination, ci::TypeDefinitionHandle type, uint32_t valueSize, const nlohmann::json& value, std::optional<int32_t> elementIndex = std::nullopt);

  private:
    const ci::SDKAPI& context;
    void* vmContext;
    std::vector<void*> ownedUserDataRequests;
};
} // namespace UserDataExtension
