# UserDataExtension 开发者指南

[English](README.md) | [简体中文](README.zh-CN.md)
[Build from source / 从源码构建](BUILDING.md)

**用 JSON 描述需要改变的数据，让多个 Mod 在同一份原生 USER 上进行增量扩展。**

UserDataExtension（UDE）是面向 Monster Hunter Wilds Mod 作者的 USER 数据扩展插件。它在游戏加载 USER 时，根据 JSON 配置追加条目、修改字段或执行数值运算，**不改写磁盘上的游戏原生 USER，也不要求分发一份完整 USER 来覆盖原版资源**。

对普通数据修改，作者只需编写 JSON 配置，无需为每个 Mod 另写 DLL。UDE 通过 ClassInjector 提供的早期加载能力，在原生数据解码完成后、游戏执行相应 `onLoad()` 初始化之前应用配置，使后续原生初始化能够使用修改后的数据。

适用环境：**Monster Hunter Wilds、Windows x64、TDB81**。运行需要配套的 ClassInjector 与 UserDataExtension 发行文件。

## 目录

- [1. 为什么使用 UDE](#1-为什么使用-ude)
- [2. 功能与适用场景](#2-功能与适用场景)
- [3. 安装与文件组织](#3-安装与文件组织)
- [4. 快速上手](#4-快速上手)
- [5. 配置结构与字段路径](#5-配置结构与字段路径)
- [6. 操作参考](#6-操作参考)
- [7. 对象、数值与资源引用](#7-对象数值与资源引用)
- [8. 加载顺序与多 Mod 协作](#8-加载顺序与多-mod-协作)
- [9. 基于公开 JSON 数据库编辑与生成](#9-基于公开-json-数据库编辑与生成)
- [10. 插件开发者接口](#10-插件开发者接口)
- [11. 排错、更新与发布](#11-排错更新与发布)
- [12. 常见问题](#12-常见问题)

## 1. 为什么使用 UDE

### 1.1 减少“多个 Mod 修改同一个文件”的冲突

传统的资源替换通常要求 Mod 分发一份修改过的完整 USER。如果 Mod A 和 Mod B 都替换同一路径，实际加载的往往只有覆盖优先级更高的那一份。即使 A 修改第一行、B 修改另一行，也可能因为整个文件互相覆盖而丢失其中一个 Mod 的效果。

UDE 把交付内容从“整个修改后的文件”变为“我需要做的操作”：

```text
原生公共 USER
  ├─ Mod A：追加自己的条目
  ├─ Mod B：修改另一条记录的一个字段
  └─ Mod C：追加其他独立条目
                  ↓
同一个已加载 USER 对象包含这些可兼容的数据修改
```

各 Mod 使用自己的 JSON 文件，在同一个原生数据对象上按确定顺序执行增量操作，不必预先制作一份包含所有 Mod 改动的合并 USER。

**这是减少整文件覆盖冲突，不是消除所有逻辑冲突。** 两个 Mod 仍可能争用同一个 ID、修改同一个字段，或对相同规则提出互斥要求。UDE 为这些情况提供明确的操作语义、执行顺序和来源日志，作者仍需协调业务意图。

### 1.2 不覆盖原生文件，保留游戏当前版本的数据基线

UDE 修改的是加载后的内存数据，而不是游戏安装中的原生 USER 文件。作者无需为少量字段修改重新分发完整二进制表，用户也可以通过移除配置并重启游戏来停止应用该修改。

这也使未涉及的数据继续来自游戏实际加载的资源。例如，补丁新增了一条原版记录，而你的配置只调整另一条记录的一个字段，就不必因为分发了旧版整表而把那条新增记录一起覆盖掉。

### 1.3 降低游戏更新后的维护成本

完整 USER 替换包容易绑定制作时的资源版本：游戏更新后，字段布局、原版内容或资源校验信息可能变化，旧文件需要重新导出、合并和保存。

UDE 以逻辑资源路径、类型名和字段名表达修改意图，针对当前加载的数据应用操作。相比重新发布一份旧版资源，它更容易保留新版本中与本 Mod 无关的更新，也便于只检查真正依赖的字段和条目。

更新适应性仍取决于目标契约：**资源改名、字段或类型变化、ID 变更，以及字段业务含义改变，都需要重新核对**。对于有稳定身份的记录，优先按 ID/GUID 等键选择，而不是依赖可能重排的数组下标。

### 1.4 用 JSON 编辑，比维护二进制差异更直接

JSON 可以用常见文本编辑器修改，并适合版本管理、代码审阅和脚本生成：

- 修改一个数值时，可以直接看到字段路径、操作和新值。
- 多人协作时，文本差异能说明改动意图。
- 相同结构的大批量条目可以由脚本生成，不必逐项操作二进制编辑器。
- 可以参考公开 JSON 数据库查找资源、类型、字段和已有记录，缩短从数据分析到配置编写的流程。

JSON 让编辑更方便，但仍需理解字段含义。UDE 校验类型和可执行性，业务效果由游戏消费者决定。

### 1.5 与整文件替换的对比

| 方面 | 完整 USER 替换 | UDE 增量配置 |
|---|---|---|
| 交付内容 | 修改后的整个资源 | 所需的字段操作和新增数据 |
| 同路径多 Mod | 通常由一个文件覆盖另一个 | 可在同一对象上合并独立操作 |
| 原版文件 | 安装覆盖资源或替换包 | 不改写、不分发该目标的整份原生 USER |
| 更新维护 | 常需重新导出和合并整表 | 检查目标路径、类型、字段及业务是否仍兼容 |
| 编辑方式 | 依赖相应资源编辑/转换工具 | 文本编辑器、JSON 工具和脚本 |
| 冲突粒度 | 主要是文件级 | 字段、记录身份及业务规则级 |
| 适用范围 | 独立资源、结构性资源制作等 | 已有 USER 的定向修改与公共表扩展 |

## 2. 功能与适用场景

### 2.1 支持的操作

| 能力 | 用途 |
|---|---|
| `append` | 向支持的一维数组或 List 追加条目，并按身份去重 |
| `set` | 为字段或集合元素赋值；集合赋值会整体替换内容 |
| `add` / `sub` | 在当前数值上增加或减少 |
| `mul` / `div` | 对当前数值应用倍率或除数 |
| `min` / `max` | 将当前数值限制为两值中的较小值或较大值 |
| 字段路径 | 访问嵌套字段、下标元素或按键匹配的记录 |
| 类型标记对象 | 用明确的类型名创建和填充新增对象 |
| 资源引用 | 为支持的 USER、Prefab 和 ResourceHolder 字段提供资源引用 |
| `"$index"` | 在新建集合元素的 Int32 字段中填入该元素的实际下标 |

### 2.2 适合使用的场景

- 多个 Mod 都需要向同一张公共参数表添加独立记录。
- 只修改既有 USER 的少量字段，而不希望交付整份资源。
- 为已有规则调整数值、范围、开关或数据引用。
- 用 JSON 组织一组可维护的数据调整，并由脚本批量生成。
- 配合其他插件，把它们需要的公共表数据作为独立配置交付。

对于 Mod 自己完整拥有的私有配置，通常直接制作独立 USER 更清晰；对于公共数据，优先复用现有条目，需要新增或定向修改时再用 UDE。UDE 不替代新模型、贴图、动作、组件实现或游戏业务注册。

新建托管类型由 ClassInjector 负责，UDE 负责为可用类型构造数据和修改已有 USER。参见 [ClassInjector 开发者指南](https://github.com/kassent/REClassInjector/blob/main/README.zh-CN.md)。

## 3. 安装与文件组织

```text
游戏根目录/
├─ hid.dll                           # ClassInjector 运行时
└─ reframework/
   ├─ plugins/
   │  ├─ UserDataExtension.dll
   │  └─ UserDataExtension.toml
   └─ data/
      └─ UserDataExtension/
         ├─ 20_WaterGunGesture/
         │  └─ 10_water_gun.json
         ├─ 30_ExampleMod/
         │  └─ 10_extra_rows.json
         └─ 90_Compatibility/
            └─ 10_adjustments.json
```

- 保留发行包提供的 `UserDataExtension.dll` 和同名 TOML，统一安装到 `reframework/plugins/`。
- 数据配置放在 `reframework/data/UserDataExtension/` 的**直属分组文件夹**内。
- 自动加载只读取各分组内的直接 `.json` 文件；不读取根层散放的 JSON，也不递归更深的子目录。
- 每个 Mod 使用独立、易识别的分组和文件名，避免被 Mod 管理器当作同路径文件覆盖。
- JSON 使用 UTF-8；采用标准 JSON 语法，不写注释、尾随逗号或重复键。

配置在启动时收集，不监视文件变化。添加、修改或移除 JSON 后，**完整退出并重启游戏**。目标资源在游戏实际加载它时才应用配置。

## 4. 快速上手

### 4.1 水枪动作：255 发弹药与 80 米射程

以修改水枪动作为例，把弹药容量从 3 发提高到 255 发，射程从 40 米增加到 80 米。两项参数都位于 `PlayerGlobalParam.user`，根类型是 `app.user_data.PlayerGlobalParam`：

**原版数据参考：**

- [在 MHWs-in-json 中查看 PlayerGlobalParam](https://github.com/dtlnor/MHWs-in-json/blob/main/natives/STM/GameDesign/Player/ActionData/Common/GlobalParam/PlayerGlobalParam.user.3.json)：查看文件路径、根类型及已有字段值。
- [打开原始 JSON](https://raw.githubusercontent.com/dtlnor/MHWs-in-json/main/natives/STM/GameDesign/Player/ActionData/Common/GlobalParam/PlayerGlobalParam.user.3.json)：适合保存到本地或在网页显示不完整时使用。

打开后搜索 `_GestureWaterGunMaxAmmo` 和 `_GestureWaterGun_Range`，对照下面的原版值与目标值，只为这两个字段生成修改。如何自行查找其他资源，见[第 9 节](#9-基于公开-json-数据库编辑与生成)。

| 功能 | 实例字段 | 类型 | 原版值 | 本例值 |
|---|---|---|---|---|
| 水枪弹药容量 | `_GestureWaterGunMaxAmmo` | `System.Byte` | 3 | 255 |
| 水枪射程 | `_GestureWaterGun_Range` | `System.Single` | 40.0 | 80.0 |

完整 USER 替换会交付整个 `PlayerGlobalParam.user.3`。使用 UDE 时，只需声明这两个字段的改值，保留当前原版资源中的其他参数。

将下面的配置保存为 `reframework/data/UserDataExtension/20_WaterGunGesture/10_water_gun.json`：

```json
{
  "format": "UserDataExtension/v1",
  "extensions": [
    {
      "target": "GameDesign/Player/ActionData/Common/GlobalParam/PlayerGlobalParam.user",
      "patches": [
        {
          "app.user_data.PlayerGlobalParam": {
            "_GestureWaterGunMaxAmmo": { "op": "set", "value": 255 },
            "_GestureWaterGun_Range": { "op": "set", "value": 80.0 }
          }
        }
      ]
    }
  ]
}
```

这里的含义是：

1. `target` 指定逻辑 USER 资源路径。
2. `app.user_data.PlayerGlobalParam` 指定该资源的完整根类型。
3. 两条字段路径直接对应弹药容量和射程，没有复制其他玩家全局参数。
4. 两个 `set` 在同一个 patch 中分别赋值；它们互不依赖，适合放在同一批次。

`System.Byte` 的数值范围为 0–255，因此本例是 **255 发容量，而不是真正无限弹药**。射程使用浮点数 `80.0`。字段名应按原样书写；本例不修改其他水枪参数，例如 `_GestureWaterGunMazzleCorrect_MaxRange`。

### 4.2 安装、检查与调整

1. 安装第 3 节的共享运行时，并将上述 JSON 放入指定分组目录。
2. 测试 UDE 配置时，停用同一路径的完整 `PlayerGlobalParam.user.3` 替换文件，使两字段修改以当前原版数据为基线。
3. 完整重启游戏，进入可使用水枪动作的场景。
4. 在 `logs/UserDataExtension.log` 中确认该配置被收集，并找到两个字段的 `Applied` 记录；有错误时按第 11 节排查。
5. 使用水枪动作，检查弹药容量和射程是否符合预期。日志写入成功与实际动作表现应分别检查。

要制作其他容量或射程版本，修改相应的 `value` 后重启游戏。例如，容量仍须在 Byte 范围内；需要较小射程时调整 `_GestureWaterGun_Range`，无需重做整个 USER 文件。

其他 Mod 若修改同一 `PlayerGlobalParam` 的不同字段，可以通过各自的 JSON 共存。若也修改这两个字段，则按加载顺序协调最终值，见第 8 节。

后续操作参考中的片段应放入相应的字段字典或 `value/items` 中。各目标使用自己的资源路径和根类型，外层配置结构沿用第 4.1 节。

## 5. 配置结构与字段路径

### 5.1 配置层级

| 层级 | 含义 |
|---|---|
| `format` | 固定为 `UserDataExtension/v1` |
| `extensions` | 本文件声明的目标扩展数组，至少一项 |
| `target` | 引擎相对的逻辑 `.user` 路径 |
| `patches` | 对目标资源执行的有序批次数组，至少一项 |
| patch 的类型键 | 目标对象的完整运行时根类型，须与加载对象匹配 |
| 类型键内的字段字典 | 字段路径到操作描述的映射 |

一个文件可以包含多个目标。`extensions` 的书写顺序不强制不同资源按该顺序加载；资源何时加载仍由游戏决定。

### 5.2 逻辑资源路径

`target` 使用如下形式：

```text
GameDesign/Player/ActionData/Common/GlobalParam/PlayerGlobalParam.user
```

省略磁盘根目录、`natives/STM/` 和 `.3` 等磁盘文件版本后缀。不使用绝对路径、`@` 包装、空路径段或 `..`。

如果参考文件是 `natives/STM/.../PlayerGlobalParam.user.3.json`，对应的 UDE `target` 是 `.../PlayerGlobalParam.user`。JSON 配置文件自身的安装路径与这个目标资源路径是两回事。

### 5.3 字段路径

| 写法 | 含义 |
|---|---|
| `_Data._ParamList` | 访问嵌套字段 |
| `_Rows[0]._Value` | 访问一维数组/List 的第一个元素及其字段 |
| `_Rows[_Id=123]._Value` | 按数字键选择记录 |
| `_Rows[_Key._Value=123]._Value` | 使用嵌套身份字段 |
| `_Rows[_Name="Alpha"]._Value` | 按字符串选择记录 |
| `_Users[$uri="GameDesign/Example/Settings.user"]` | 按 USER 引用的 URI 选择元素 |

示意路径中的 `_Rows`、`_Id` 等应替换为目标类型真实字段。字段名区分大小写，使用 TDB 实例字段名，不使用编辑器翻译标签。把含引号的路径作为 JSON 键时，对内部引号使用 `\"` 转义。

- 下标从 0 开始，越界报错，不自动扩容。
- 等值选择器要求一个匹配，零匹配或多匹配都报错。
- 中间父对象应已存在；路径访问不自动补建整个对象链。
- 选择器是单键等值匹配，不提供通配符、递归查询或表达式计算。
- 外部 USER 是独立资源，修改其内部字段时为它单独声明 `target`，不从当前资源沿引用跨入。

## 6. 操作参考

### 6.1 `append`：追加并保留原有数据

对象集合需要指定去重身份 `key`。以公共参数表 `GameDesign/Enemy/CommonData/Data/EnemyDangerData.user`（根类型 `app.user_data.EmParamDanger`）为例，以下是其 `_Data._ParamList` 的操作片段：

```json
{
  "op": "append",
  "key": "_EnumValue",
  "items": [
    {
      "app.user_data.EmParamDanger.cDangerValueData": {
        "_EnumValue": 900001,
        "_Value": 400,
        "_AddValue": 50,
        "_MaxValue": 600,
        "_ShapePreset": "00000000-0000-0000-0000-000000000000"
      }
    }
  ]
}
```

本例的 ID、数值和空 GUID 仅示意数据结构。实际新增行应根据消费者要求填齐字段，选择适用的预设引用，并为 Mod 分配不会与其他内容争用的身份。新增参数行本身不注册新怪物或其他业务对象。

- 已有相同身份时保留已有行，跳过本次候选行，不合并候选字段。
- 同一 `items` 内重复身份报错。
- `key` 可以是嵌套字段路径；USER 引用集合可使用 `"$uri"`。
- 标量、字符串、枚举、GUID 及支持的小值类型集合按值去重，不填写 `key`。
- 末端集合为空时可以追加；为 null 时，须能从类型声明确定其集合和元素类型。

修改已有记录使用 `set` 或数值运算，不依赖 `append` 覆盖它。

### 6.2 `set` 与数值运算

| 操作 | 结果 | 示例 |
|---|---|---|
| `set` | 赋值 | `{ "op": "set", "value": 400 }` |
| `add` | 当前值 + value | `{ "op": "add", "value": 20 }` |
| `sub` | 当前值 - value | `{ "op": "sub", "value": 20 }` |
| `mul` | 当前值 × value | `{ "op": "mul", "value": 1.2 }` |
| `div` | 当前值 ÷ value | `{ "op": "div", "value": 2 }` |
| `min` | 取当前值与 value 中较小者，相当于上限 | `{ "op": "min", "value": 600 }` |
| `max` | 取当前值与 value 中较大者，相当于下限 | `{ "op": "max", "value": 0 }` |

`set` 可以处理支持的标量、对象引用、集合及值类型；引用字段允许 `null`。**对集合使用 `set` 是整体替换，不是追加**，需要保留原内容时使用 `append`。

算术仅适用于普通数值。整数赋值和运算操作数要求整数，并检查范围和溢出；整数除法向零取整。浮点值要求有限且在目标范围内。除零报错；枚举、Boolean 和 GUID 不参与算术。

### 6.3 有依赖的操作拆成多个 patch

一个 patch 内的字段操作是互不依赖的一批，JSON 字典的键顺序不表示执行先后。

以下 `patches` 数组表达“先加 20，再乘 2”：

```json
[
  {
    "app.user_data.EmParamDanger": {
      "_Data._ParamList[_EnumValue=900001]._Value": { "op": "add", "value": 20 }
    }
  },
  {
    "app.user_data.EmParamDanger": {
      "_Data._ParamList[_EnumValue=900001]._Value": { "op": "mul", "value": 2 }
    }
  }
]
```

“先追加一行，再按键修改新行”“先创建父对象，再访问子字段”等同样拆成前后 patch。不要在同一 patch 中整体替换一个对象，又同时修改它的子字段。

### 6.4 `"$index"`：填写实际元素位置

如果新建元素的 Int32 字段需要保存它在集合中的位置，可填写 `"$index"`：

```json
{
  "Example.Row": {
    "_Id": 900001,
    "_Index": "$index"
  }
}
```

这是类型结构示意。`Example.Row` 应替换为实际类型，且 `_Index` 必须是 Int32。追加位置按当前集合有效长度和已接受的新元素计算；被去重跳过的元素不占位。内层集合从自己的 0 开始计数。

只有处于新建集合元素上下文的 Int32 值可以使用该标记，不把它用作数值身份 key。字符串字段中的 `"$index"` 仍是普通文本。

## 7. 对象、数值与资源引用

### 7.1 类型标记对象

新增对象采用 `{"完整类型名":{"字段":值}}` 的形式，类型名须与目标字段/集合元素兼容。引用集合可以声明实际派生类型；内嵌值类型使用匹配的具体类型。

普通集合字段的值使用 JSON 数组，不额外包装 `elementType` 或操作级的 `items`。新增对象仅填写所声明字段的初始数据，不自动复制其他现有对象的参数；业务要求的字段由作者提供。

### 7.2 数据类型

| 数据 | JSON 写法与要求 |
|---|---|
| Boolean | `true` / `false` |
| 整数 | 范围内的整数，或精确十进制整数字符串 |
| Single/Double | 有限数值 |
| 枚举 | 真实底层数值，或可解析的数字文本，不以显示名代替身份 |
| 字符串 | 普通 JSON 字符串 |
| GUID | 标准 GUID 字符串 |
| 引用 | 类型标记对象、适用的资源表示，或 `null` |
| 数组/List | 元素值组成的 JSON 数组 |

使用会把大整数转为双精度数的编辑器或生成器时，Int64/UInt64 建议写成十进制字符串以保留精度。最终值仍须在真实字段类型范围内。

支持整值/集合操作的小值类型包括 `via.vec2/vec3/vec4`、`via.Quaternion`、`via.Float2/Float3/Float4`、`via.Color`、`via.Range/RangeI`、`app.MOTION_SEQUENCE_FILTER_ID` 和已有的 Nullable Int32/UInt32。使用各类型真实成员名，例如：

```json
{
  "via.vec3": { "x": 1.0, "y": 2.0, "z": 3.0 }
}
```

复杂结构体优先通过成员字段路径修改；任意类型的整体赋值或集合元素布局并非都受支持。

### 7.3 资源引用

引用已有 USER 时使用 `$uri`：

```json
{
  "Example.Settings": {
    "$uri": "GameDesign/Example/Settings.user"
  }
}
```

将示意类型和路径替换为真实 USER 类型及资源。`$uri` 与普通字段初始化分开使用，资源内容由原生加载流程提供；引用到的资源可能异步解码。

Prefab 使用专用表示：

```json
{
  "via.Prefab": {
    "Standby": false,
    "Path": "GameDesign/Example/Object.pfb"
  }
}
```

ResourceHolder 派生字段可使用资源路径字符串，由字段类型决定实际 holder。普通字符串不会仅因内容像路径就自动成为资源请求。

## 8. 加载顺序与多 Mod 协作

### 8.1 自动配置顺序

分组和组内文件按 A–Z 的不区分大小写序数规则排序，相同时以原始名称比较。数字采用字典序，建议用 `10_`、`20_`、`90_` 等补零前缀，而不是依赖自然数字排序。

```text
10_Base/10_parameters.json
20_ExampleMod/10_parameters.json
90_Compatibility/10_adjustments.json
```

同一文件内按 `extensions`、`patches` 数组顺序处理。单个 patch 内的字段操作不建立相互依赖。

### 8.2 重叠修改的结果

| 情况 | 行为 |
|---|---|
| 两个 Mod 追加不同身份 | 可以共存，保留原有行 |
| `append` 遇到已有身份 | 保留先存在的条目，跳过候选条目 |
| 不同来源 `set` 同一字段 | 后执行的值生效，相应写入冲突可在日志中定位 |
| 对同一数值连续算术 | 按实际顺序基于当前结果计算，顺序可能影响结果 |
| 两个 Mod 使用同一个 JSON 安装路径 | 文件系统/Mod 管理器仍可能在 UDE 读取前覆盖文件 |
| 不同路径的 JSON 内容相同 | 作为独立来源执行，不按内容自动合并 |

使用唯一分组名、稳定条目身份和最小字段修改范围。需要兼容补丁时，把协调操作放在明确的后置分组，并说明涉及的 Mod 和期望顺序。UDE 的冲突日志帮助定位实际重叠写入，不代替作者判断所有业务兼容性。

### 8.3 批次失败与生效时机

UDE 在原生解码后的数据上工作：

```text
启动时收集 JSON
        ↓
游戏按需加载原生 USER
        ↓
UDE 应用匹配资源和根类型的 patches
        ↓
游戏继续 onLoad 等初始化，建立其运行数据
```

配置不会主动加载每一个 `target`，也不追溯修改已经加载完的对象。缓存查询不重复执行配置；目标资源重新实际解码时，会针对新对象再次应用。

每个 patch 先定位和校验，再准备修改结果，最后提交。定位或准备失败时不提交该批；提交阶段异常可能保留已完成的部分。某个根对象应用失败后，会停止该对象余下的 UDE 操作。没有跨 patch、文件或资源的整体回滚。

## 9. 基于公开 JSON 数据库编辑与生成

### 9.1 在 GitHub 网页中找到文件

以第 4 节的水枪动作为例，可以从 [dtlnor/MHWs-in-json](https://github.com/dtlnor/MHWs-in-json) 找到对应的玩家全局参数导出：

1. 打开仓库首页，确认正在查看的分支或提交版本。
2. 使用 **Go to file**，输入 `PlayerGlobalParam`，从结果中打开 `PlayerGlobalParam.user.3.json`。该功能按文件名/路径找文件，不是搜索字段内容；参见 [GitHub 文件查找说明](https://docs.github.com/en/search-github/searching-on-github/finding-files-on-github)。
3. 也可按目录进入下列路径，或直接打开 [PlayerGlobalParam 文件页](https://github.com/dtlnor/MHWs-in-json/blob/main/natives/STM/GameDesign/Player/ActionData/Common/GlobalParam/PlayerGlobalParam.user.3.json)：

```text
natives/STM/
  GameDesign/Player/ActionData/Common/GlobalParam/
    PlayerGlobalParam.user.3.json
```

4. 打开文件后查找 `GestureWaterGun`，重点查看 `_GestureWaterGunMaxAmmo` 和 `_GestureWaterGun_Range`。文件较大或网页展示不完整时，打开 [原始 JSON](https://raw.githubusercontent.com/dtlnor/MHWs-in-json/main/natives/STM/GameDesign/Player/ActionData/Common/GlobalParam/PlayerGlobalParam.user.3.json)，保存后用文本编辑器搜索。

如果只知道字段名，不知道文件名，可在 GitHub 搜索中使用下面的代码搜索条件。`repo:` 将搜索限制到该仓库，`path:` 可进一步限制路径；参见 [GitHub 代码搜索语法](https://docs.github.com/en/search-github/github-code-search/understanding-github-code-search-syntax)。

```text
repo:dtlnor/MHWs-in-json "_GestureWaterGunMaxAmmo"
repo:dtlnor/MHWs-in-json path:PlayerGlobalParam.user.3.json GestureWaterGun
```

网页代码搜索需要登录或没有返回结果时，可以使用下面的本地搜索流程。文件存在与字段存在应分别检查；不同版本的导出可能包含不同字段。

### 9.2 下载后在本地搜索

可以下载仓库 ZIP，或使用 Git 获取数据。解压后的数据根目录中应能直接看到 `natives/`；为配合下文示例，将该根目录命名为 `MHWs-in-json`。

安装 Git 后，可在工作目录执行：

```powershell
git clone https://github.com/dtlnor/MHWs-in-json.git
git -C MHWs-in-json ls-files "*PlayerGlobalParam*"
git -C MHWs-in-json grep -n -i "GestureWaterGun" -- "*.json"
```

第一条命令下载仓库，只需执行一次；第二条按文件名找路径；第三条搜索 JSON 内容并输出文件名和行号。下载 ZIP 时，也可直接用编辑器“在文件中查找”搜索 `GestureWaterGun`。换成其他字段名、资源名或记录身份，就能定位其他修改目标。

### 9.3 从导出内容识别类型和字段路径

打开文件后，不只是记录新数值，还需要确定 UDE 的三个定位信息：

| 从参考数据获得的信息 | 水枪示例 | 在 UDE 中的用途 |
|---|---|---|
| 仓库文件路径 | `natives/STM/GameDesign/Player/ActionData/Common/GlobalParam/PlayerGlobalParam.user.3.json` | 转换为逻辑 `target` |
| 外层对象的完整类型键 | `app.user_data.PlayerGlobalParam` | 作为 patch 的根类型键 |
| 根对象中的实际字段名 | `_GestureWaterGunMaxAmmo`、`_GestureWaterGun_Range` | 作为字段路径 |

路径转换时，删除 `natives/STM/` 前缀、最后的 `.json` 和文件版本后缀 `.3`，保留 `.user`：

```text
参考文件：natives/STM/GameDesign/Player/ActionData/Common/GlobalParam/PlayerGlobalParam.user.3.json
UDE target：GameDesign/Player/ActionData/Common/GlobalParam/PlayerGlobalParam.user
```

完整类型名通常位于导出数组中某个对象的类型键位置。不要把文件名当成类型名，也不要把导出根数组的 `[0]` 写入 UDE 字段路径。

对于嵌套数据，路径由**真实字段名**连接而成，导出中的类型包装键不属于字段路径。例如参数表中 `_Data` 对象被一个类型键包裹，内部还有 `_ParamList` 字段，UDE 路径仍写 `_Data._ParamList`。选择具体记录时，再加入 `[_EnumValue=...]` 这样的稳定身份选择器，而不是直接照抄导出数组的位置。

确认同名字段的含义也很重要。水枪示例只修改弹药容量和射程，不因为 `_GestureWaterGunMazzleCorrect_MaxRange` 同样含有 `Range` 就把它一起改动。字段类型和允许范围仍按目标游戏版本的类型信息检查；JSON 中的数值外观本身不能区分 Byte、Int32 等存储类型。

### 9.4 从参考数据生成需要的差异

水枪配置的编写过程是：

1. 在匹配版本的数据中确认原值：弹药容量为 3，射程为 40.0。
2. 确定目标值：255 和 80.0。
3. 仅为这两个字段生成 `set`，不复制其余玩家全局参数。
4. 把字段操作放入 `app.user_data.PlayerGlobalParam` 根类型，再封装成第 4 节的完整配置。

得到的字段字典如下：

```json
{
  "_GestureWaterGunMaxAmmo": { "op": "set", "value": 255 },
  "_GestureWaterGun_Range": { "op": "set", "value": 80.0 }
}
```

随后保存到 `reframework/data/UserDataExtension/20_WaterGunGesture/10_water_gun.json`，重启游戏并检查两个字段的应用记录和动作表现。无需编辑数据库文件、覆盖原始 USER 或重新打包它。

对于新增表行，参考已有同类行的字段结构，为新行分配独立身份，改用 `append` 和相应 `key`。对于修改已有行，使用唯一身份选择器配合 `set` 或算术操作。引用字段中的导出表示也要转换为 UDE 对应的资源写法，例如第 7.3 节的 `$uri`，不直接复制所有导出元数据。

**导出 JSON 是参考数据，UDE JSON 是操作声明。** 不把整份数据库导出直接放进 UDE 配置目录。若所查看版本没有目标字段，先核对仓库版本和目标游戏版本，必要时使用目标版本的资源导出；不根据相似名字猜测字段。发布时记录参考提交和支持的游戏版本。

数字字段可以解析 `[32]显示名称` 形式中的数字部分；显示名称不参与身份校验。生成配置时优先输出真实数值，并保持普通字符串字段原文。

### 9.5 用脚本生成水枪配置

下面的离线 Python 示例读取下载的 `PlayerGlobalParam` JSON，检查根类型及字段，再生成与第 4 节相同的两字段配置。它输出一个 Mod 文件夹，不修改游戏文件。

准备好包含相应字段的 `MHWs-in-json/` 数据目录，将脚本保存为 `generate_ude.py` 后运行：

```python
import json
from pathlib import Path

SOURCE = Path(
    "MHWs-in-json/natives/STM/GameDesign/Player/ActionData/Common/"
    "GlobalParam/PlayerGlobalParam.user.3.json"
)
OUTPUT = Path(
    "MyMod/reframework/data/UserDataExtension/20_WaterGunGesture/10_water_gun.json"
)
ROOT_TYPE = "app.user_data.PlayerGlobalParam"
TARGET = "GameDesign/Player/ActionData/Common/GlobalParam/PlayerGlobalParam.user"
CHANGES = {
    "_GestureWaterGunMaxAmmo": 255,
    "_GestureWaterGun_Range": 80.0,
}

export = json.loads(SOURCE.read_text(encoding="utf-8-sig"))
roots = export if isinstance(export, list) else [export]
matches = [item[ROOT_TYPE] for item in roots
           if isinstance(item, dict) and ROOT_TYPE in item]
if len(matches) != 1:
    raise ValueError("Expected exactly one PlayerGlobalParam root")
root = matches[0]
if type(root.get("_GestureWaterGunMaxAmmo")) is not int:
    raise ValueError("Expected an integer ammo-capacity field")
if type(root.get("_GestureWaterGun_Range")) not in (int, float):
    raise ValueError("Expected a numeric range field")

config = {
    "format": "UserDataExtension/v1",
    "extensions": [{
        "target": TARGET,
        "patches": [{
            ROOT_TYPE: {field: {"op": "set", "value": value}
                        for field, value in CHANGES.items()}
        }]
    }]
}
OUTPUT.parent.mkdir(parents=True, exist_ok=True)
OUTPUT.write_text(json.dumps(config, ensure_ascii=False, indent=2) + "\n",
                  encoding="utf-8")
print(f"Generated: {OUTPUT}")
```

```text
python generate_ude.py
```

生成后审阅输出 JSON，将 `MyMod/reframework/` 中的内容按目录结构安装，并在目标场景检查效果。复杂批量生成也应保留同样原则：**参考原始数据、校验输入结构、只输出需要的差异**，而不是重新分发整张原表。

## 10. 插件开发者接口

普通 JSON Mod 使用自动加载即可。需要在启动时动态生成配置的 C++ 插件，可以使用 `UserDataExtension.h` 提供的两个入口：

| 接口 | 输入 | 用途 |
|---|---|---|
| `RegisterUserDataExtension(const char* jsonPathUtf8)` | 配置文件的 UTF-8 绝对路径 | 登记固定目录之外或由插件管理的文件 |
| `RegisterUserDataExtensionJson(const char* sourceIdUtf8, const char* jsonUtf8)` | 来源 ID 与 NUL 结尾的 UTF-8 JSON 文本 | 直接登记生成的配置，无需中间文件 |

- 在调用方的 CI 函数登记阶段同步调用，并声明对 UDE 的模块依赖，确保服务先装载。
- 登记窗口在 UDE 开始处理 TDB-ready 通知时关闭；不等待普通 RF 插件初始化或运行中回调再登记。
- 返回 `true` 表示配置解析和登记成功，目标资源稍后加载时才应用。
- 接口同步复制所需内容，返回后调用方可以释放输入字符串。
- 同一规范化绝对文件路径只保留首次成功登记的内容；同一内存来源 ID 也保留首次成功快照，ID 区分大小写。
- 内存来源按 ID 区分大小写排序，先于文件来源执行。API 登记的文件与自动发现的文件统一按路径排序，不以调用时间获取优先级。

调用方与 UDE、ClassInjector 使用配套发行接口。对于纯配置 Mod，分发 JSON 而不增加专用 DLL，通常更简单。

## 11. 排错、更新与发布

### 11.1 排查顺序

1. 检查 `hid.dll`、`UserDataExtension.dll` 与同名 TOML 的安装位置和配套版本。
2. 检查 JSON 是否放在直属分组目录中，且文件扩展名、编码和语法正确。
3. 查看 `logs/UserDataExtension.log` 中的来源、顺序、登记和类型/字段错误；使用 Mod 管理器时按其重定向规则查找日志。
4. 核对 `target`、实际根类型、字段名、选择器身份及数据范围。
5. 确认目标 USER 确实由游戏加载，再检查操作日志和实际业务结果。
6. 检查其他 Mod 是否使用了相同 JSON 路径、记录身份、字段或完整 USER 替换资源。

| 日志片段 | 含义 |
|---|---|
| `Registered` | 配置来源已收集 |
| `Order=` | 最终来源执行顺序 |
| `rejected source=` | 类型/操作校验失败，查看具体原因 |
| `Applied` | 字段操作已执行；追加包含 added/skipped 数量 |
| `Conflict append` | 新增身份重叠，已有条目保留 |
| `Conflict set` | 来源写入重叠，按实际顺序取后值 |
| `stopped USER=` | 该根对象的后续操作已停止，查看失败 patch 和字段 |

### 11.2 游戏更新后

检查路径和类型是否仍存在、字段类型与语义是否变化、选择器是否仍唯一，以及原版是否新增了你需要保留的内容。尤其检查按物理下标定位的配置和对集合使用 `set` 的配置。

### 11.3 发布与卸载

发布内容应包括独立分组内的 JSON、必要的其他资源，以及运行时依赖、支持游戏版本、修改目标和冲突说明。共享 UDE 运行时与各 Mod 的数据配置分开管理。

发布前检查单 Mod 的预期效果、多 Mod 共存、目标资源重新加载以及正常切场景/退出。卸载时删除本 Mod 的配置和自有文件后重启；其他 Mod 仍依赖的共享运行时继续保留。普通 UDE 内存修改不会写回 USER，但通过游戏业务触发的存档等持久化行为仍按该业务规则处理。

## 12. 常见问题

### 是否彻底解决了所有 Mod 冲突？

UDE 降低整文件替换冲突，让独立操作能够共存。同字段争用、相同条目身份、同名 JSON 路径以及互斥业务规则仍需要协调。

### 游戏更新后一定无需修改配置吗？

配置基于当前加载的数据，更容易保留原版更新。但资源、类型、字段、ID 或规则改变时仍需调整并验证。

### 是否需要解包和重新打包 USER？

对于 UDE 支持的已有 USER 数据修改，只交付 JSON 操作，不需要重新生成或打包目标 USER。查明字段含义可能需要公开数据库、类型资料或相应查看工具；新增私有资源仍需正常制作。

### 把公开数据库的 JSON 复制进目录就可以了吗？

数据库导出用来查结构和参考数据。提取差异并封装成 UDE 操作格式后再安装，见第 9 节。

### `append` 能更新同 ID 的已有行吗？

`append` 保留已有身份的记录。要更新字段，使用唯一选择器配合 `set` 或数值运算；要先新增再调整，使用前后两个 patch。

### 编辑 JSON 后为什么没有变化？

完整重启游戏，并进入会加载目标 USER 的功能。随后按日志检查目录、目标路径、类型、字段和操作结果。Lua 重载或编辑文件本身不重新收集 UDE 配置。

### UDE 能定义新类型或直接生成新玩法吗？

类型由 ClassInjector 注册，玩法由组件或业务代码实现。UDE 负责构造适用的数据、请求资源引用和修改已有 USER；新增条目还需对应的原生消费者和业务接入。

---

**实践要点：以当前原版数据为基线，用最小 JSON 差异表达意图，为每个 Mod 使用独立来源和稳定身份，并在真实业务中检查最终结果。**
