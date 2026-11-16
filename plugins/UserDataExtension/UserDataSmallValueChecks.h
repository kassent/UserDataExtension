#pragma once
#include "UserDataCollection.h"
#include "UserDataObjectBuilder.h"
#include "UserDataPatchExecutor.h"
#include <ci/API.hpp>
#include <cstring>
#include <stdexcept>

// 同一组真实 TDB 场景用于离线替身与原生 VM；不依赖 fixture 特有对象头或手写字段偏移。
namespace UserDataSmallValueTests {
using Json = nlohmann::json;

// 从字段字典组装生产执行器使用的 patch，不经过第二套实现。
inline UserDataExtension::TypePatch MakeTypePatch(const ci::SDKAPI& context, const char* rootTypeName, const Json& fieldDeclarations) {
    using namespace UserDataExtension;
    TypePatch typePatch{.rootTypeName = rootTypeName, .rootType = context.tdb->find_type(rootTypeName)};
    for (const auto& [path, description] : fieldDeclarations.items()) {
        FieldEdit fieldEdit{.fieldPath = path, .fieldPathSteps = ParseFieldPath(path), .kind = ParseFieldEditKind(description.at("op").get<std::string>())};
        if (description.contains("value"))
            fieldEdit.value = description.at("value");
        if (description.contains("items"))
            fieldEdit.items = description.at("items");
        if (description.contains("key")) {
            fieldEdit.identityKey = description.at("key").get<std::string>();
            if (fieldEdit.identityKey != "$uri")
                fieldEdit.identityKeySteps = ParseFieldPath(fieldEdit.identityKey);
        }
        typePatch.fieldEdits.push_back(std::move(fieldEdit));
    }
    return typePatch;
}

// 按生产字段路径读取纯值，用于核对真实 VM 与离线元数据两种后端。
template <class T>
inline T Read(const ci::SDKAPI& context, void* vmctx, void* root, const std::string& path) {
    const auto field = UserDataExtension::ResolveFieldPath(context, vmctx, root, UserDataExtension::ParseFieldPath(path));
    if (field.valueSize != sizeof(T))
        throw std::runtime_error("unexpected read layout: " + path);
    T value;
    std::memcpy(&value, field.address, sizeof(value));
    return value;
}

// 验证纯值写入、List 原生 Add、数组拼接、padding/正负零去重和不支持类型的边界。
// vmctx 必须来自真实托管调用作用域；离线替身后端允许传 nullptr。
inline Json Run(const ci::SDKAPI& context, void* vmctx) {
    using namespace UserDataExtension;
    size_t checks = 0;
    auto Check = [&checks](bool value, const char* reason) {
        ++checks;
        if (!value)
            throw std::runtime_error(reason);
    };
    auto Reject = [&Check](auto action) {
        bool rejected = false;
        try {
            action();
        } catch (const std::exception&) {
            rejected = true;
        }
        Check(rejected, "unsupported operation was accepted");
    };
    std::vector<void*> roots;
    ObjectBuilder builder(context, vmctx);
    auto Create = [&](const Json& declaration) {
        auto* root = builder.CreateObject(context.tdb->find_type(declaration.begin().key().c_str()), declaration);
        context.managed_object->add_ref(root); // 完整初始化后才首次持有。
        roots.push_back(root);
        return root;
    };
    auto Apply = [&](void* root, const char* type, const Json& fields) {
        const auto typePatch = MakeTypePatch(context, type, fields);
        ValidateTypePatch(context, typePatch);
        PatchExecutor(context, vmctx, root).ApplyTypePatch(typePatch, "SmallValueChecks");
    };
    auto Collection = [&](void* root, const std::string& path) {
        const auto field = ResolveFieldPath(context, vmctx, root, ParseFieldPath(path));
        return ReadCollection(context, field.type, *static_cast<void**>(field.address));
    };
    try {
        const char* types[]{"via.vec2", "via.vec3", "via.vec4", "via.Quaternion", "via.Float2", "via.Float3", "via.Float4", "via.Color", "via.Range", "via.RangeI", "app.MOTION_SEQUENCE_FILTER_ID", "System.Nullable`1<System.Int32>", "System.Nullable`1<System.UInt32>"};
        for (const auto* name : types) {
            const auto* type = context.tdb->find_type(name);
            const auto* layout = FindSupportedValueLayout(context, type);
            Check(layout && layout->size == context.type_definition->get_valuetype_size(type), "small value type/size missing");
        }
        Check(context.type_definition->get_valuetype_size(context.tdb->find_type("System.String")) == 0 && context.type_definition->get_valuetype_size(nullptr) == 0, "CI value-size query accepted reference/null");
        const auto samples = Json::parse(R"(
        [
            {
                "via.vec2": {
                    "x": 1,
                    "y": 2
                }
            },
            {
                "via.vec3": {
                    "x": 1,
                    "y": 2,
                    "z": 3
                }
            },
            {
                "via.vec4": {
                    "x": 1,
                    "y": 2,
                    "z": 3,
                    "w": 4
                }
            },
            {
                "via.Quaternion": {
                    "x": 1,
                    "y": 2,
                    "z": 3,
                    "w": 4
                }
            },
            {
                "via.Float2": {
                    "x": 1,
                    "y": 2
                }
            },
            {
                "via.Float3": {
                    "x": 1,
                    "y": 2,
                    "z": 3
                }
            },
            {
                "via.Float4": {
                    "x": 1,
                    "y": 2,
                    "z": 3,
                    "w": 4
                }
            },
            {
                "via.Color": {
                    "rgba": 987
                }
            },
            {
                "via.Range": {
                    "s": 1,
                    "r": 2
                }
            },
            {
                "via.RangeI": {
                    "s": 1,
                    "r": 2
                }
            },
            {
                "app.MOTION_SEQUENCE_FILTER_ID": {
                    "PageNo": 3,
                    "FilterNo": 4
                }
            },
            {
                "System.Nullable`1<System.Int32>": {
                    "_HasValue": true,
                    "_Value": 13
                }
            },
            {
                "System.Nullable`1<System.UInt32>": {
                    "_HasValue": true,
                    "_Value": 14
                }
            }
        ]
        )");
        for (const auto& sample : samples) {
            const auto* type = context.tdb->find_type(sample.begin().key().c_str());
            const auto size = GetSupportedValueSize(context, type);
            alignas(16) uint8_t bytes[16]{};
            ValidateJsonValue(context, type, size, sample);
            builder.WriteValue(nullptr, bytes, type, size, sample);
            for (const auto& [name, expected] : sample.begin().value().items()) {
                const auto member = ci::API::TypeDefinition{&context, type}.find_field(name.c_str());
                if (member.get_type().handle == context.tdb->find_type("System.Single")) {
                    float value;
                    std::memcpy(&value, bytes + member.get_offset_from_fieldptr(), sizeof(value));
                    Check(value == expected.get<float>(), "float value encoding failed");
                } else if (member.get_type().handle == context.tdb->find_type("System.Boolean")) {
                    Check(bool(bytes[member.get_offset_from_fieldptr()]) == expected.get<bool>(), "Boolean value encoding failed");
                } else {
                    uint32_t value;
                    std::memcpy(&value, bytes + member.get_offset_from_fieldptr(), sizeof(value));
                    Check(value == expected.get<uint32_t>(), "integer value encoding failed");
                }
            }
        }

        auto* cloth = Create(Json::parse(R"(
        {
            "app.user_data.UnderwaterClothParameter": {
                "_Gravity": {
                    "via.vec3": {
                        "x": 1,
                        "y": 2,
                        "z": 3
                    }
                },
                "_ForceDirRotationOffset": {
                    "via.Quaternion": {
                        "w": 1
                    }
                },
                "_ForceCycleDuration": {
                    "via.Range": {
                        "s": 1,
                        "r": 2
                    }
                }
            }
        }
        )"));
        Apply(cloth, "app.user_data.UnderwaterClothParameter", Json::parse(R"(
        {
            "_Gravity": {
                "op": "set",
                "value": {
                    "via.vec3": {
                        "x": 10
                    }
                }
            },
            "_ForceDirRotationOffset": {
                "op": "set",
                "value": {
                    "via.Quaternion": {
                        "x": 0.5,
                        "w": 0.5
                    }
                }
            },
            "_ForceCycleDuration": {
                "op": "set",
                "value": {
                    "via.Range": {
                        "s": 3,
                        "r": 4
                    }
                }
            }
        }
        )"));
        Check(Read<float>(context, vmctx, cloth, "_Gravity.x") == 10 && Read<float>(context, vmctx, cloth, "_Gravity.y") == 0, "whole vec3 set did not zero omitted members");
        Check(Read<float>(context, vmctx, cloth, "_ForceDirRotationOffset.w") == 0.5f && Read<float>(context, vmctx, cloth, "_ForceCycleDuration.r") == 4, "Quaternion/Range whole set failed");
        Apply(cloth, "app.user_data.UnderwaterClothParameter", Json::parse(R"(
        {
            "_Gravity.y": {
                "op": "set",
                "value": 9
            }
        }
        )"));
        Check(Read<float>(context, vmctx, cloth, "_Gravity.x") == 10 && Read<float>(context, vmctx, cloth, "_Gravity.y") == 9, "member set damaged adjacent value");
        const auto bad = MakeTypePatch(context, "app.user_data.UnderwaterClothParameter", Json::parse(R"(
        {
            "_ForceCoef": {
                "op": "set",
                "value": 42
            },
            "_Gravity": {
                "op": "set",
                "value": {
                    "via.vec3": {
                        "x": "invalid"
                    }
                }
            }
        }
        )"));
        Reject([&] {
            PatchExecutor(context, vmctx, cloth).ApplyTypePatch(bad, "prepare-failure");
        });
        Check(Read<float>(context, vmctx, cloth, "_ForceCoef") == 0 && Read<float>(context, vmctx, cloth, "_Gravity.x") == 10, "failed preparation committed partial writes");
        Reject([&] {
            ValidateTypePatch(context, MakeTypePatch(context, "app.user_data.UnderwaterClothParameter", Json::parse(R"(
            {
                "_Gravity": {
                    "op": "add",
                    "value": 1
                }
            }
            )")));
        });

        auto* attack = Create(Json::parse(R"(
        {
            "app.user_data.AttackAreaParam": {
                "attackAreaDataList": [
                    {
                        "app.AttackAreaDataPack": {
                            "DataPack": [
                                {
                                    "app.AttackAreaDataList": {
                                        "_DataList": [
                                            {
                                                "app.AttackAreaData": {
                                                    "RequestSetID": {
                                                        "System.Nullable`1<System.UInt32>": {
                                                            "_HasValue": true,
                                                            "_Value": 2
                                                        }
                                                    },
                                                    "ShellIndex": {
                                                        "System.Nullable`1<System.Int32>": {
                                                            "_HasValue": true,
                                                            "_Value": 4
                                                        }
                                                    },
                                                    "Obb": {
                                                        "via.OBB": {
                                                            "extent": {
                                                                "via.vec3": {
                                                                    "x": 1,
                                                                    "y": 2,
                                                                    "z": 3
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        ],
                                        "_RootMove": {
                                            "app.RootMoveData": {
                                                "_PosList": [
                                                    {
                                                        "via.vec3": {
                                                            "x": 0,
                                                            "y": 2,
                                                            "z": 3
                                                        }
                                                    },
                                                    {
                                                        "via.vec3": {
                                                            "x": 4,
                                                            "y": 5,
                                                            "z": 6
                                                        }
                                                    }
                                                ],
                                                "_RotList": [
                                                    {
                                                        "via.Quaternion": {
                                                            "w": 1
                                                        }
                                                    }
                                                ]
                                            }
                                        }
                                    }
                                }
                            ]
                        }
                    }
                ]
            }
        }
        )"));
        const std::string prefix = "attackAreaDataList[0].DataPack[0].";
        const auto pos = prefix + "_RootMove._PosList", rot = prefix + "_RootMove._RotList", data = prefix + "_DataList[0].";
        const auto original = Collection(attack, pos);
        Check(original.stride == 16 && original.count == 2, "vec3 List stride/count mismatch");
        std::memset(original.items->elements + 12, 0xA5, 4); // 已核 vec3 padding，不影响 x/y/z。
        Json listOps;
        listOps[pos] = {{"op", "append"}, {"items", Json::array({Json{{"via.vec3", {{"x", -0.0}, {"y", 2}, {"z", 3}}}}, Json{{"via.vec3", {{"x", 7}, {"y", 8}, {"z", 9}}}}})}};
        listOps[rot] = {{"op", "append"}, {"items", Json::parse(R"(
        [
            {
                "via.Quaternion": {
                    "w": 1
                }
            },
            {
                "via.Quaternion": {
                    "x": 1
                }
            }
        ]
        )")}};
        Apply(attack, "app.user_data.AttackAreaParam", listOps);
        Check(Collection(attack, pos).list == original.list && Collection(attack, pos).count == 3, "List append replaced owner or failed value dedup");
        Check(Read<float>(context, vmctx, attack, pos + "[2].z") == 9 && Collection(attack, rot).count == 2 && Read<float>(context, vmctx, attack, rot + "[1].x") == 1, "native value List Add/readback failed");
        Apply(attack, "app.user_data.AttackAreaParam", listOps);
        Check(Collection(attack, pos).count == 3 && Collection(attack, rot).count == 2, "value append replay was not idempotent");
        Json elementOps;
        elementOps[pos + "[0].x"] = {{"op", "set"}, {"value", 12}};
        elementOps[pos + "[1]"] = {{"op", "set"}, {"value", Json{{"via.vec3", {{"y", 11}}}}}};
        elementOps[data + "RequestSetID"] = {{"op", "set"}, {"value", Json{{"System.Nullable`1<System.UInt32>", {{"_HasValue", false}}}}}};
        elementOps[data + "ShellIndex"] = {{"op", "set"}, {"value", Json{{"System.Nullable`1<System.Int32>", {{"_HasValue", true}, {"_Value", 7}}}}}};
        elementOps[data + "Obb.extent.x"] = {{"op", "set"}, {"value", 99}};
        Apply(attack, "app.user_data.AttackAreaParam", elementOps);
        Check(Read<float>(context, vmctx, attack, pos + "[0].x") == 12 && Read<float>(context, vmctx, attack, pos + "[0].y") == 2 && Read<float>(context, vmctx, attack, pos + "[1].x") == 0 && Read<float>(context, vmctx, attack, pos + "[1].y") == 11, "value element/member writes failed");
        Check(!Read<bool>(context, vmctx, attack, data + "RequestSetID._HasValue") && Read<int32_t>(context, vmctx, attack, data + "ShellIndex._Value") == 7, "Nullable set failed");
        Check(Read<float>(context, vmctx, attack, data + "Obb.extent.x") == 99, "existing large-value member support regressed");
        Reject([&] {
            GetSupportedValueSize(context, context.tdb->find_type("via.OBB"));
        });
        Json replace;
        replace[pos] = {{"op", "set"}, {"value", Json::parse(R"(
        [
            {
                "via.vec3": {
                    "z": 15
                }
            }
        ]
        )")}};
        Apply(attack, "app.user_data.AttackAreaParam", replace);
        Check(Collection(attack, pos).count == 1 && Read<float>(context, vmctx, attack, pos + "[0].z") == 15, "value List replacement failed");

        auto* effect = Create(Json::parse(R"(
        {
            "via.relib.effect.userdata.EffectMaterialGroupParameter": {
                "ElementList": [
                    {
                        "via.relib.effect.userdata.EffectMaterialGroupParameter.Element": {
                            "GroupName": "existing",
                            "Color": {
                                "via.Color": {
                                    "rgba": 123
                                }
                            },
                            "ColorExt": [
                                {
                                    "via.Color": {
                                        "rgba": 123
                                    }
                                }
                            ]
                        }
                    }
                ]
            }
        }
        )"));
        Apply(effect, "via.relib.effect.userdata.EffectMaterialGroupParameter", Json::parse(R"(
        {
            "ElementList[0].Color": {
                "op": "set",
                "value": {
                    "via.Color": {
                        "rgba": 456
                    }
                }
            },
            "ElementList[0].ColorExt": {
                "op": "append",
                "items": [
                    {
                        "via.Color": {
                            "rgba": 123
                        }
                    },
                    {
                        "via.Color": {
                            "rgba": 789
                        }
                    }
                ]
            }
        }
        )"));
        Check(Collection(effect, "ElementList[0].ColorExt").count == 2 && Read<uint32_t>(context, vmctx, effect, "ElementList[0].ColorExt[1].rgba") == 789 && Read<uint32_t>(context, vmctx, effect, "ElementList[0].Color.rgba") == 456, "Color array append/whole set failed");
        // 同一 USER 中的 via.* 引用 List 仍走对象 Add，旧行身份和内容都必须保留。
        const auto originalElements = Collection(effect, "ElementList");
        Check(originalElements.count == 1, "reference List fixture count mismatch");
        auto* originalElement = *reinterpret_cast<void**>(originalElements.items->elements);
        const auto referenceAppend = Json::parse(R"(
        {
            "ElementList": {
                "op": "append",
                "key": "GroupName",
                "items": [
                    {
                        "via.relib.effect.userdata.EffectMaterialGroupParameter.Element": {
                            "GroupName": "existing",
                            "Color": {
                                "via.Color": {
                                    "rgba": 999
                                }
                            }
                        }
                    },
                    {
                        "via.relib.effect.userdata.EffectMaterialGroupParameter.Element": {
                            "GroupName": "added",
                            "Color": {
                                "via.Color": {
                                    "rgba": 567
                                }
                            }
                        }
                    }
                ]
            }
        }
        )");
        Apply(effect, "via.relib.effect.userdata.EffectMaterialGroupParameter", referenceAppend);
        const auto appendedElements = Collection(effect, "ElementList");
        Check(appendedElements.count == 2 && appendedElements.list == originalElements.list, "reference List append replaced owner or failed count");
        Check(*reinterpret_cast<void**>(appendedElements.items->elements) == originalElement && Read<uint32_t>(context, vmctx, effect, "ElementList[0].Color.rgba") == 456 && Read<uint32_t>(context, vmctx, effect, "ElementList[1].Color.rgba") == 567, "via reference List append lost original/new row");
        Apply(effect, "via.relib.effect.userdata.EffectMaterialGroupParameter", referenceAppend);
        Check(Collection(effect, "ElementList").count == 2, "reference List append replay was not idempotent");

        // TDB 中 Color List 与 Nullable 数组的持有类不是 USER；仅直接测试生产执行器。
        // 这些新建对象不接入游戏业务，也不把非 USER 根伪装成可登记的配置。
        auto* hunterEffect = Create(Json::parse(R"(
        {
            "app.cHunterEffect": {
                "_AmuletColors": [
                    {
                        "via.Color": {
                            "rgba": 123
                        }
                    }
                ]
            }
        }
        )"));
        const auto originalColors = Collection(hunterEffect, "_AmuletColors");
        const auto colorPatch = MakeTypePatch(context, "app.cHunterEffect", Json::parse(R"(
        {
            "_AmuletColors": {
                "op": "append",
                "items": [
                    {
                        "via.Color": {
                            "rgba": 123
                        }
                    },
                    {
                        "via.Color": {
                            "rgba": 456
                        }
                    }
                ]
            }
        }
        )"));
        Reject([&] {
            ValidateTypePatch(context, colorPatch);
        });
        PatchExecutor colorExecutor(context, vmctx, hunterEffect);
        colorExecutor.ApplyTypePatch(colorPatch, "ColorList");
        const auto colorsView = Collection(hunterEffect, "_AmuletColors");
        Check(colorsView.count == 2 && colorsView.list == originalColors.list, "Color List append count/owner mismatch");
        // 先验长再读元素；追加回归时测试应明确失败，而不是越界读取第二项。
        Check(Read<uint32_t>(context, vmctx, hunterEffect, "_AmuletColors[0].rgba") == 123 && Read<uint32_t>(context, vmctx, hunterEffect, "_AmuletColors[1].rgba") == 456, "production Color List.Add indirect ABI failed");
        colorExecutor.ApplyTypePatch(colorPatch, "ColorList");
        Check(Collection(hunterEffect, "_AmuletColors").count == 2, "Color List append replay was not idempotent");

        auto* actionArg = Create(Json::parse(R"(
        {
            "app.cHunterSubActionArg": {
                "_MarkedValue": [
                    {
                        "System.Nullable`1<System.Int32>": {
                            "_HasValue": false,
                            "_Value": 123
                        }
                    },
                    {
                        "System.Nullable`1<System.Int32>": {
                            "_HasValue": true,
                            "_Value": 1
                        }
                    }
                ]
            }
        }
        )"));
        const auto nullablePatch = MakeTypePatch(context, "app.cHunterSubActionArg", Json::parse(R"(
        {
            "_MarkedValue": {
                "op": "append",
                "items": [
                    {
                        "System.Nullable`1<System.Int32>": {
                            "_HasValue": false,
                            "_Value": 999
                        }
                    },
                    {
                        "System.Nullable`1<System.Int32>": {
                            "_HasValue": true,
                            "_Value": 7
                        }
                    }
                ]
            }
        }
        )"));
        PatchExecutor nullableExecutor(context, vmctx, actionArg);
        nullableExecutor.ApplyTypePatch(nullablePatch, "NullableArray");
        Check(Collection(actionArg, "_MarkedValue").count == 3, "Nullable empty values were not deduplicated");
        Check(!Read<bool>(context, vmctx, actionArg, "_MarkedValue[0]._HasValue") && Read<int32_t>(context, vmctx, actionArg, "_MarkedValue[0]._Value") == 123 && Read<bool>(context, vmctx, actionArg, "_MarkedValue[2]._HasValue") && Read<int32_t>(context, vmctx, actionArg, "_MarkedValue[2]._Value") == 7, "Nullable append changed inactive payload or lost new value");
        nullableExecutor.ApplyTypePatch(nullablePatch, "NullableArray");
        Check(Collection(actionArg, "_MarkedValue").count == 3, "Nullable append replay was not idempotent");
        Json result{{"checks", checks}, {"types", std::size(types)}, {"vec3_stride", 16}, {"list_identity_preserved", true}, {"passed", true}};
        for (auto* root : roots)
            context.managed_object->release(root);
        return result;
    } catch (...) {
        for (auto* root : roots)
            context.managed_object->release(root);
        throw;
    }
}
} // namespace UserDataSmallValueTests
