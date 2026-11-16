# UserDataExtension Developer Guide

[English](README.md) | [简体中文](README.zh-CN.md)
[Build from source / 从源码构建](BUILDING.md)

**Describe data changes in JSON so that multiple mods can extend the same native USER incrementally.**

UserDataExtension (UDE) is a USER data-extension plugin for Monster Hunter Wilds mod authors. When the game loads a USER, UDE applies JSON declarations to append entries, change fields, or perform arithmetic. **It does not rewrite native USER files on disk or require a complete replacement USER to be distributed.**

For ordinary data changes, authors only need JSON configuration; each mod does not need its own DLL. Using ClassInjector's early loading capability, UDE applies configuration after native data decoding and before the corresponding `onLoad()` initialization, allowing subsequent native initialization to consume the modified data.

Supported environment: **Monster Hunter Wilds, Windows x64, TDB81**. Matching ClassInjector and UserDataExtension release files are required.

## Contents

- [1. Why Use UDE?](#1-why-use-ude)
- [2. Features and Use Cases](#2-features-and-use-cases)
- [3. Installation and File Layout](#3-installation-and-file-layout)
- [4. Quick Start](#4-quick-start)
- [5. Configuration Structure and Field Paths](#5-configuration-structure-and-field-paths)
- [6. Operation Reference](#6-operation-reference)
- [7. Objects, Values, and Resource References](#7-objects-values-and-resource-references)
- [8. Loading Order and Multi-Mod Cooperation](#8-loading-order-and-multi-mod-cooperation)
- [9. Editing and Generation from a Public JSON Database](#9-editing-and-generation-from-a-public-json-database)
- [10. Plugin Developer API](#10-plugin-developer-api)
- [11. Troubleshooting, Updates, and Distribution](#11-troubleshooting-updates-and-distribution)
- [12. Frequently Asked Questions](#12-frequently-asked-questions)

## 1. Why Use UDE?

### 1.1 Reduce Conflicts Between Mods Editing the Same File

Traditional resource replacement typically distributes an entire modified USER. If Mod A and Mod B replace the same path, the higher-priority replacement usually wins. Even when A edits one row and B edits another, replacing the whole file can discard one mod's changes.

UDE changes the deliverable from an entire modified file to a description of the required operations:

```text
Native shared USER
  +-- Mod A: append its own entries
  +-- Mod B: change one field in another record
  +-- Mod C: append other independent entries
                    |
                    v
The same loaded USER object contains the compatible changes
```

Each mod supplies its own JSON files. Independent operations run on the same native data object in a defined order, without requiring a premerged USER containing every mod's changes.

**This reduces whole-file replacement conflicts; it does not eliminate every logical conflict.** Mods may still compete for an ID, write the same field, or require mutually exclusive rules. UDE supplies explicit operation semantics, deterministic ordering, and source-aware logs; authors still coordinate the intended behavior.

### 1.2 Keep Native Files Intact and Use the Game's Current Data Baseline

UDE modifies loaded data in memory, not native USER files in the game installation. Authors do not need to redistribute a complete binary table for a small field change. Users can stop applying a change by removing its configuration and restarting the game.

Unchanged data continues to come from the resource the game actually loads. For example, if an update adds a built-in record while your configuration adjusts one field in another record, your mod does not overwrite that addition by shipping an older complete table.

### 1.3 Reduce Maintenance After Game Updates

A complete USER replacement is often tied to the resource version used to create it. After an update changes layouts, built-in content, or resource validation information, the old file may need to be exported, merged, and saved again.

UDE expresses intent through logical resource paths, type names, and field names, then applies operations to the currently loaded data. Compared with distributing an older resource, this makes it easier to preserve unrelated game updates and review only the fields and entries the mod depends on.

Compatibility still depends on the target contract: **resource renames, field/type changes, changed IDs, and changed field semantics all require review**. Prefer stable IDs or GUIDs over physical array positions that may be reordered.

### 1.4 Edit JSON Instead of Maintaining Binary Differences

JSON works with ordinary text editors and is suitable for version control, review, and scripted generation:

- A numeric change directly shows the field path, operation, and new value.
- Text diffs communicate intent during collaboration.
- Scripts can generate large sets of similarly structured entries without repetitive binary-editor work.
- Public JSON databases help locate resources, types, fields, and existing records, shortening the path from data inspection to configuration authoring.

JSON makes editing more convenient, but authors still need to understand field semantics. UDE checks types and whether operations can execute; the consuming game system determines their effects.

### 1.5 Comparison with Whole-File Replacement

| Aspect | Complete USER replacement | UDE incremental configuration |
|---|---|---|
| Deliverable | An entire modified resource | Required field operations and new data |
| Multiple mods targeting one path | One file typically overrides another | Independent operations can coexist on one object |
| Native files | Install replacement resources/packages | No rewriting or redistribution of the entire target native USER |
| Update maintenance | Often requires re-exporting and merging the table | Review target paths, types, fields, and behavior for compatibility |
| Editing | Resource editing/conversion tools | Text editors, JSON tools, and scripts |
| Conflict granularity | Primarily file-level | Fields, record identities, and game rules |
| Best fit | Independent resources and structural resource authoring | Targeted changes to existing USERs and shared-table extensions |

## 2. Features and Use Cases

### 2.1 Supported Operations

| Capability | Purpose |
|---|---|
| `append` | Append entries to supported one-dimensional arrays or Lists, deduplicating by identity |
| `set` | Assign a field or collection element; assigning a collection replaces its contents |
| `add` / `sub` | Increase or decrease the current value |
| `mul` / `div` | Apply a multiplier or divisor to the current value |
| `min` / `max` | Keep the smaller or larger of the current value and operand |
| Field paths | Access nested fields, indexed elements, or records matched by a key |
| Type-tagged objects | Create and populate new objects using explicit type names |
| Resource references | Supply references for supported USER, Prefab, and ResourceHolder fields |
| `"$index"` | Fill an Int32 field in a newly created collection element with its actual index |

### 2.2 Suitable Uses

- Several mods need to add independent records to the same shared parameter table.
- A mod changes only a few fields of an existing USER and should not distribute the entire resource.
- Existing rules need numeric, range, flag, or data-reference adjustments.
- A maintainable set of data changes should be organized in JSON and generated in batches.
- Data required by another plugin should be distributed as separate shared-table configuration.

For private configuration wholly owned by a mod, a separate USER is often clearer. Reuse existing shared entries first; use UDE when adding entries or making targeted changes is necessary. UDE does not replace new models, textures, motions, component implementations, or game-system registration.

ClassInjector defines new managed types; UDE constructs data for available types and modifies existing USERs. See the [ClassInjector Developer Guide](https://github.com/kassent/REClassInjector/blob/main/README.md).

## 3. Installation and File Layout

```text
Game root/
├─ hid.dll                           # ClassInjector runtime
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

- Keep the release package's `UserDataExtension.dll` and matching TOML together in `reframework/plugins/`.
- Put data configuration inside **direct child group folders** of `reframework/data/UserDataExtension/`.
- Automatic discovery reads only direct `.json` files in each group. It does not read loose JSON files in the root or recurse into deeper subdirectories.
- Give each mod a distinct, recognizable group and filenames so that a mod manager does not override them as same-path files.
- Use UTF-8 and standard JSON syntax: no comments, trailing commas, or duplicate keys.

Configuration is collected at startup; file changes are not watched. **Fully exit and restart the game** after adding, editing, or removing JSON. A target is patched when the game actually loads that resource.

## 4. Quick Start

### 4.1 Water Gun Gesture: 255 Shots and an 80-Metre Range

This example modifies the Water Gun gesture, increasing ammo capacity from 3 to 255 shots and range from 40 to 80 metres. Both parameters are in `PlayerGlobalParam.user`, whose root type is `app.user_data.PlayerGlobalParam`:

**Original data references:**

- [View PlayerGlobalParam in MHWs-in-json](https://github.com/dtlnor/MHWs-in-json/blob/main/natives/STM/GameDesign/Player/ActionData/Common/GlobalParam/PlayerGlobalParam.user.3.json) to inspect the file path, root type, and existing field values.
- [Open the raw JSON](https://raw.githubusercontent.com/dtlnor/MHWs-in-json/main/natives/STM/GameDesign/Player/ActionData/Common/GlobalParam/PlayerGlobalParam.user.3.json) to save a local copy or view the data when the file page is incomplete.

Search for `_GestureWaterGunMaxAmmo` and `_GestureWaterGun_Range`, compare the original and target values below, and generate changes for these two fields only. See [Section 9](#9-editing-and-generation-from-a-public-json-database) for finding other resources yourself.

| Feature | Instance field | Type | Original value | Example value |
|---|---|---|---|---|
| Water Gun ammo capacity | `_GestureWaterGunMaxAmmo` | `System.Byte` | 3 | 255 |
| Water Gun range | `_GestureWaterGun_Range` | `System.Single` | 40.0 | 80.0 |

A whole-file replacement distributes the entire `PlayerGlobalParam.user.3`. With UDE, declare only these two field changes and retain the other parameters from the current native resource.

Save as `reframework/data/UserDataExtension/20_WaterGunGesture/10_water_gun.json`, or use the included [Water Gun example JSON](plugins/UserDataExtension/examples/water-gun.json):

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

This means:

1. `target` identifies the logical USER resource path.
2. `app.user_data.PlayerGlobalParam` identifies its fully qualified root type.
3. The two field paths directly identify ammo capacity and range without copying other player-global parameters.
4. Both `set` operations assign independent fields, making them suitable for the same patch.

`System.Byte` has a range of 0–255, so this gives **a capacity of 255 shots, not genuinely unlimited ammunition**. Range uses the floating-point value `80.0`. Preserve field names exactly. Other Water Gun parameters, such as `_GestureWaterGunMazzleCorrect_MaxRange`, are left unchanged.

### 4.2 Install, Check, and Adjust

1. Install the shared runtime from Section 3 and place the JSON in the specified group folder.
2. When testing the UDE configuration, disable the complete `PlayerGlobalParam.user.3` replacement at the same resource path so that these two edits use current native data as their baseline.
3. Fully restart and enter a scene where the Water Gun gesture can be used.
4. In `logs/UserDataExtension.log`, confirm that the configuration was collected and find `Applied` records for both fields. Follow Section 11 if errors occur.
5. Use the Water Gun gesture and check ammo capacity and range. Check successful data writes and actual gesture behavior separately.

To make another capacity or range variant, change the relevant `value` and restart. Capacity must remain within the Byte range; adjust `_GestureWaterGun_Range` for a shorter range without rebuilding the entire USER.

Other mods editing different fields of the same `PlayerGlobalParam` can coexist through their own JSON files. If they also edit these two fields, coordinate the final values through loading order; see Section 8.

Later operation-reference fragments belong in the relevant field dictionary or `value/items`. Each target uses its own resource path and root type, with the outer configuration structure from Section 4.1.

## 5. Configuration Structure and Field Paths

### 5.1 Configuration Layers

| Layer | Meaning |
|---|---|
| `format` | Fixed to `UserDataExtension/v1` |
| `extensions` | Array of target extensions declared by this file; at least one |
| `target` | Engine-relative logical `.user` path |
| `patches` | Ordered array of operation batches for the target; at least one |
| Patch type key | Fully qualified runtime root type; must match the loaded object |
| Field dictionary inside the type key | Maps field paths to operation descriptions |

A file can contain multiple targets. The order of `extensions` does not force different resources to load in that order; the game still controls resource loading.

### 5.2 Logical Resource Paths

Use a `target` such as:

```text
GameDesign/Player/ActionData/Common/GlobalParam/PlayerGlobalParam.user
```

Omit the drive/game root, `natives/STM/`, and disk file-version suffixes such as `.3`. Do not use absolute paths, `@` wrappers, empty path segments, or `..`.

For a reference file named `natives/STM/.../PlayerGlobalParam.user.3.json`, the UDE `target` is `.../PlayerGlobalParam.user`. The JSON configuration's installation path is separate from the target resource path.

### 5.3 Field Paths

| Syntax | Meaning |
|---|---|
| `_Data._ParamList` | Access nested fields |
| `_Rows[0]._Value` | Access the first array/List element and its field |
| `_Rows[_Id=123]._Value` | Select a record by a numeric key |
| `_Rows[_Key._Value=123]._Value` | Use a nested identity field |
| `_Rows[_Name="Alpha"]._Value` | Select by a string |
| `_Users[$uri="GameDesign/Example/Settings.user"]` | Select a USER reference by URI |

Replace illustrative fields such as `_Rows` and `_Id` with actual fields. Names are case-sensitive TDB instance field names, not translated editor labels. When a quoted path is a JSON key, escape its inner quotes as `\"`.

- Indices start at 0. Out-of-range access is an error and does not grow the collection.
- Equality selectors require one match. Zero or multiple matches are errors.
- Intermediate parent objects must exist; path traversal does not construct an entire missing object chain.
- Selectors provide single-key equality, not wildcards, recursive queries, or expression evaluation.
- External USERs are independent resources. Declare a separate `target` to edit their fields rather than traversing into them from another resource.

## 6. Operation Reference

### 6.1 `append`: Add Entries While Preserving Existing Data

Object collections require an identity `key` for deduplication. For the shared table `GameDesign/Enemy/CommonData/Data/EnemyDangerData.user` (root type `app.user_data.EmParamDanger`), this is an operation fragment for `_Data._ParamList`:

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

The ID, values, and empty GUID illustrate the structure. Real rows need fields required by their consumers, appropriate preset references, and an identity that does not compete with other content. A parameter row alone does not register a new monster or another game entity.

- If an identity already exists, keep its row and skip the candidate; candidate fields are not merged.
- Duplicate identities within the same `items` are errors.
- `key` can be a nested field path. USER-reference collections can use `"$uri"`.
- Scalars, strings, enums, GUIDs, and supported small value types deduplicate by value; omit `key`.
- Empty collections can be appended to. For a null collection, its collection and element types must be identifiable from the type declaration.

Use `set` or arithmetic to edit existing records rather than expecting `append` to overwrite them.

### 6.2 `set` and Arithmetic

| Operation | Result | Example |
|---|---|---|
| `set` | Assign a value | `{ "op": "set", "value": 400 }` |
| `add` | Current value + value | `{ "op": "add", "value": 20 }` |
| `sub` | Current value - value | `{ "op": "sub", "value": 20 }` |
| `mul` | Current value × value | `{ "op": "mul", "value": 1.2 }` |
| `div` | Current value ÷ value | `{ "op": "div", "value": 2 }` |
| `min` | Smaller of current value and value; an upper bound | `{ "op": "min", "value": 600 }` |
| `max` | Larger of current value and value; a lower bound | `{ "op": "max", "value": 0 }` |

`set` handles supported scalars, object references, collections, and value types; reference fields accept `null`. **Setting a collection replaces it rather than appending.** Use `append` to preserve existing contents.

Arithmetic applies only to ordinary numeric fields. Integer assignments and arithmetic operands require integers and are checked for range and overflow; integer division truncates toward zero. Floating values must be finite and within the target range. Division by zero is an error. Enums, Boolean, and GUID do not support arithmetic.

### 6.3 Separate Dependent Operations into Patches

Fields in one patch form an independent batch. JSON object-key ordering does not define execution dependencies.

This `patches` array means “add 20, then multiply by 2”:

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

Likewise, use successive patches to append a row and then select it, or create a parent object and then access a child field. Do not replace an entire object and modify its child fields in the same patch.

### 6.4 `"$index"`: Fill the Actual Element Position

If an Int32 field in a newly created element must store its collection position, use `"$index"`:

```json
{
  "Example.Row": {
    "_Id": 900001,
    "_Index": "$index"
  }
}
```

This is a type-shape illustration. Replace `Example.Row` with an actual type, and ensure `_Index` is Int32. Append positions use the current valid collection length plus accepted new elements. Deduplicated candidates do not consume positions. Nested collections start their own index at 0.

The marker applies only to Int32 values inside newly created collection elements. Do not use it as a numeric identity key. In a string field, `"$index"` remains ordinary text.

## 7. Objects, Values, and Resource References

### 7.1 Type-Tagged Objects

New objects use `{"Fully.Qualified.TypeName":{"Field":value}}`. The type must be compatible with the destination field or collection element. Reference collections may specify a concrete derived type; embedded value types use the matching concrete type.

Ordinary collection fields contain JSON arrays, without additional `elementType` or operation-level `items` wrappers. New objects receive the declared initial data, not automatic copies of another object's parameters. Authors supply fields required by the consuming system.

### 7.2 Data Types

| Data | JSON representation and requirements |
|---|---|
| Boolean | `true` / `false` |
| Integer | An in-range integer or exact decimal integer string |
| Single/Double | A finite numeric value |
| Enum | Actual underlying numeric value or parseable numeric text, not a display name as identity |
| String | An ordinary JSON string |
| GUID | A standard GUID string |
| Reference | A type-tagged object, applicable resource representation, or `null` |
| Array/List | A JSON array of element values |

If an editor or generator converts large integers to double precision, prefer decimal strings for Int64/UInt64 to preserve precision. Values must still fit the actual field type.

Small value types supported for whole-value/collection operations include `via.vec2/vec3/vec4`, `via.Quaternion`, `via.Float2/Float3/Float4`, `via.Color`, `via.Range/RangeI`, `app.MOTION_SEQUENCE_FILTER_ID`, and existing Nullable Int32/UInt32 types. Use their actual member names, for example:

```json
{
  "via.vec3": { "x": 1.0, "y": 2.0, "z": 3.0 }
}
```

Prefer member field paths for complex structs. Whole-value assignment and collection layouts are not supported for arbitrary types.

### 7.3 Resource References

Use `$uri` to reference an existing USER:

```json
{
  "Example.Settings": {
    "$uri": "GameDesign/Example/Settings.user"
  }
}
```

Replace the illustrative type and path with an actual USER type and resource. Keep `$uri` separate from ordinary field initialization. The native loading flow supplies resource contents, potentially asynchronously.

Prefabs use this representation:

```json
{
  "via.Prefab": {
    "Standby": false,
    "Path": "GameDesign/Example/Object.pfb"
  }
}
```

ResourceHolder-derived fields can use resource path strings, with the field type determining the holder. An ordinary string does not become a resource request merely because it resembles a path.

## 8. Loading Order and Multi-Mod Cooperation

### 8.1 Automatic Configuration Order

Groups and files within groups use case-insensitive ordinal A–Z sorting, with original names breaking ties. Numbers sort lexically. Use zero-padded prefixes such as `10_`, `20_`, and `90_` rather than relying on natural numeric ordering.

```text
10_Base/10_parameters.json
20_ExampleMod/10_parameters.json
90_Compatibility/10_adjustments.json
```

Within a file, `extensions` and `patches` follow array order. Fields in one patch do not establish dependencies on each other.

### 8.2 Overlapping Changes

| Situation | Behavior |
|---|---|
| Two mods append different identities | Can coexist while preserving original rows |
| `append` encounters an existing identity | Keep the existing entry and skip the candidate |
| Different sources `set` the same field | The later value takes effect; relevant write conflicts can be located in logs |
| Consecutive arithmetic on one field | Each operation uses the current result; ordering may matter |
| Two mods install JSON at the same path | The filesystem/mod manager may override a file before UDE reads it |
| Identical JSON at different paths | Executes as independent sources, not automatically merged by content |

Use distinct group names, stable record identities, and the smallest necessary set of fields. Put compatibility adjustments in an explicitly later group and document affected mods and ordering. Conflict logs help identify actual overlapping writes; they do not decide every aspect of gameplay compatibility.

### 8.3 Batch Failures and Application Timing

UDE operates on decoded native data:

```text
Collect JSON at startup
          |
          v
The game loads a native USER when needed
          |
          v
UDE applies patches matching its resource path and root type
          |
          v
The game continues onLoad and other initialization of runtime data
```

Configuration does not proactively load every `target` or retroactively patch already loaded objects. Cached resource lookups do not reapply it. If a resource is actually decoded again, its new objects receive the configuration again.

Each patch first locates and validates fields, then prepares results, then commits. Location or preparation failure does not commit that batch. A commit-stage exception may leave completed writes in place. A failure stops remaining UDE operations for that root object. There is no all-or-nothing rollback across patches, files, or resources.

## 9. Editing and Generation from a Public JSON Database

### 9.1 Find the File on GitHub

For the Water Gun gesture in Section 4, locate the corresponding player-global parameter export in [dtlnor/MHWs-in-json](https://github.com/dtlnor/MHWs-in-json):

1. Open the repository and note the branch or revision being viewed.
2. Use **Go to file**, enter `PlayerGlobalParam`, and open `PlayerGlobalParam.user.3.json`. This searches filenames/paths, not field contents; see [GitHub's file-finder instructions](https://docs.github.com/en/search-github/searching-on-github/finding-files-on-github).
3. Alternatively, browse the path below or open the [PlayerGlobalParam file page](https://github.com/dtlnor/MHWs-in-json/blob/main/natives/STM/GameDesign/Player/ActionData/Common/GlobalParam/PlayerGlobalParam.user.3.json):

```text
natives/STM/
  GameDesign/Player/ActionData/Common/GlobalParam/
    PlayerGlobalParam.user.3.json
```

4. Search the file for `GestureWaterGun`, focusing on `_GestureWaterGunMaxAmmo` and `_GestureWaterGun_Range`. If the file is large or the page is incomplete, open the [raw JSON](https://raw.githubusercontent.com/dtlnor/MHWs-in-json/main/natives/STM/GameDesign/Player/ActionData/Common/GlobalParam/PlayerGlobalParam.user.3.json), save it, and search in a text editor.

If you know a field but not its file, use these GitHub code-search queries. `repo:` limits the repository; `path:` further narrows the path. See [GitHub's code-search syntax](https://docs.github.com/en/search-github/github-code-search/understanding-github-code-search-syntax).

```text
repo:dtlnor/MHWs-in-json "_GestureWaterGunMaxAmmo"
repo:dtlnor/MHWs-in-json path:PlayerGlobalParam.user.3.json GestureWaterGun
```

If web code search requires sign-in or returns no results, use the local workflow below. Check file existence and field existence separately; exported fields can differ between versions.

### 9.2 Download and Search Locally

Download the repository ZIP or obtain the data with Git. Its extracted data root should directly contain `natives/`; name that root directory `MHWs-in-json` to follow the examples.

With Git installed, run these commands from your working directory:

```powershell
git clone https://github.com/dtlnor/MHWs-in-json.git
git -C MHWs-in-json ls-files "*PlayerGlobalParam*"
git -C MHWs-in-json grep -n -i "GestureWaterGun" -- "*.json"
```

The first downloads the repository once. The second finds paths by filename. The third searches JSON contents and reports filenames and line numbers. For a ZIP download, an editor's Find in Files can search `GestureWaterGun` directly. Substitute another field, resource name, or record identity to locate other targets.

### 9.3 Identify Types and Field Paths in the Export

Record three pieces of information, not just the desired new values:

| Information from the reference | Water Gun example | UDE use |
|---|---|---|
| Repository file path | `natives/STM/GameDesign/Player/ActionData/Common/GlobalParam/PlayerGlobalParam.user.3.json` | Convert to the logical `target` |
| Fully qualified outer object type key | `app.user_data.PlayerGlobalParam` | Patch root type key |
| Actual fields in the root object | `_GestureWaterGunMaxAmmo`, `_GestureWaterGun_Range` | Field paths |

Remove `natives/STM/`, the final `.json`, and the `.3` file-version suffix, retaining `.user`:

```text
Reference: natives/STM/GameDesign/Player/ActionData/Common/GlobalParam/PlayerGlobalParam.user.3.json
UDE target: GameDesign/Player/ActionData/Common/GlobalParam/PlayerGlobalParam.user
```

The fully qualified type name is usually a type key on an object inside the exported array. Do not substitute the filename for the type name or include the export root array's `[0]` in a UDE field path.

For nested data, join **actual field names**; exported type-wrapper keys are not path segments. For example, if `_Data` contains a type-wrapped object with a `_ParamList` field, the UDE path is still `_Data._ParamList`. Add a stable identity selector such as `[_EnumValue=...]` to select a row instead of copying its export array position.

Check similarly named fields carefully. The Water Gun example changes capacity and range, not `_GestureWaterGunMazzleCorrect_MaxRange` simply because its name also contains `Range`. Verify storage types and limits against the target game's type information: a number's JSON appearance alone does not distinguish Byte from Int32.

### 9.4 Turn Reference Data into the Required Differences

For the Water Gun configuration:

1. Confirm original values in matching-version data: capacity 3 and range 40.0.
2. Choose target values: 255 and 80.0.
3. Generate `set` operations for these two fields only, without copying other player-global parameters.
4. Place them under `app.user_data.PlayerGlobalParam` and wrap them in the complete configuration from Section 4.

The resulting field dictionary is:

```json
{
  "_GestureWaterGunMaxAmmo": { "op": "set", "value": 255 },
  "_GestureWaterGun_Range": { "op": "set", "value": 80.0 }
}
```

Save the configuration as `reframework/data/UserDataExtension/20_WaterGunGesture/10_water_gun.json`, restart, and check both field application records and gesture behavior. Editing the database, replacing the original USER, or repacking it is unnecessary.

For new table rows, reference similar entries, assign a distinct identity, and use `append` with the appropriate `key`. For existing rows, use a unique identity selector with `set` or arithmetic. Convert exported reference representations into UDE's resource syntax, such as `$uri` in Section 7.3, rather than copying all export metadata unchanged.

**Exported JSON is reference data; UDE JSON declares operations.** Do not install complete database exports as UDE configuration. If a field is missing, check the repository revision and target game version and obtain a matching-version resource export when needed. Avoid guessing from similar names. Document the reference commit and supported game version when publishing.

Numeric fields can parse the numeric part of display values such as `[32]DisplayName`. Display names do not validate identities. Prefer explicit numeric values in generated configuration and preserve ordinary string fields as text.

### 9.5 Generate the Water Gun Configuration with a Script

This offline Python example reads the downloaded `PlayerGlobalParam` JSON, checks the root and fields, and generates the same two-field configuration as Section 4. It writes a mod folder, not game files.

Prepare a `MHWs-in-json/` data directory containing the required fields, save the script as `generate_ude.py`, and run it:

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

Review the generated JSON, install the contents of `MyMod/reframework/` preserving their layout, and check effects in the target scene. More complex generators should follow the same principle: **reference original data, validate input structure, and output only the required differences**, rather than redistributing an entire original table.

## 10. Plugin Developer API

### 10.1 Registration Contract

Ordinary JSON mods use automatic loading. C++ plugins generating startup configuration use the two C exports in [UserDataExtension.h](plugins/UserDataExtension/UserDataExtension.h), both returning `bool`, using `__cdecl`, and declared `noexcept`:

| API | Input | Purpose |
|---|---|---|
| `RegisterUserDataExtension(const char* jsonPathUtf8)` | Absolute UTF-8 path to a configuration file | Register files outside the automatic directory or managed by a plugin |
| `RegisterUserDataExtensionJson(const char* sourceIdUtf8, const char* jsonUtf8)` | Source ID and NUL-terminated UTF-8 JSON text | Submit generated configuration without an intermediate file |

- Prefer **synchronous submission from `ci_on_initialize`**. CI types can be queried, and all first-round callbacks finish before UDE's second-round callback.
- Pure file declarations that need no TDB queries can also be submitted during `CI_REGISTER_PROVIDER`. That stage must not access the VM/RF SDK; it is not the only registration opportunity.
- UDE closes the API window in `FinalizeRegistration` during `ci_on_post_initialize`, then scans directories, combines and sorts sources, binds types, and installs the hook. Do not submit from your own post callback, asynchronous tasks, ordinary RF initialization, or in-game callbacks.
- `true` means **syntax parsing and registration succeeded**, not successful type binding, resource loading, or application. Binding failures log `rejected source=`. Calls after the window closes return `false`.
- Inputs are saved as snapshots; release input strings after return. A normalized absolute path keeps its first successful content. In-memory IDs are case-sensitive and keep their first successful snapshot; failed registrations do not claim the ID.
- In-memory sources sort case-sensitively by ID before file sources. API files and scanned files share path-based ordering. Registration call order does not set override priority.

### 10.2 From CI Startup to Native USER Consumption

```text
CI collects provider function/enum registrations and type declarations
  → Prepare TDB, native entries, and custom vtables
  → Every provider's ci_on_initialize
       Callers submit UDE JSON (CI metadata queries are available)
  → Every provider's ci_on_post_initialize
       UDE: FinalizeRegistration → validate declarations → InstallLoadingHook
  → The game continues static construction and normal resource loading
  → A USER's internal object graph actually finishes decoding
       Match resource URI + exact root type → prepare and commit field edits
  → Native onLoad builds runtime caches from the modified data
```

Per-provider order within a round is not an inter-plugin protocol. The **boundary between rounds** guarantees first-round submission before UDE closes registration. UDE itself exports only the post notification; an empty first notification is unnecessary. Registration neither loads all targets proactively nor replays `onLoad` or directly fills native caches.

### 10.3 Compilable C++ Submission Example

This zero-type provider submits Section 4's Water Gun configuration. Compile as C++20 for Windows x64, adding `plugins/ClassInjector/include` and `plugins/UserDataExtension` to include paths. Install `ExamplePlugin.dll` with an empty same-directory `ExamplePlugin.toml`, plus matching UDE DLL/TOML files. No RF SDK or UDE import library is required.

UDE and its caller may belong to different MO2 mods or physical directories. Explicitly load the **game-visible absolute path** and resolve the public export. Do not search only beside the caller's physical DLL, change global DLL search paths, or depend on manifest filenames. This follows the loading approach already used by [EnemyRegistry](https://github.com/kassent/EnemyRegistry/blob/main/plugins/EnemyRegistry/Plugin.cpp).

```cpp
#include <Windows.h>
#include <ci/Provider.hpp>
#include <UserDataExtension.h>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iterator>

CI_REGISTER_PROVIDER(registration) {
    // Zero-type provider: no VM or RF SDK access here.
}

extern "C" __declspec(dllexport)
void ci_on_initialize(const ci::ProviderContext* context) noexcept {
    (void)context;
    try {
        wchar_t executablePath[32768]{};
        const auto length = GetModuleFileNameW(
            nullptr, executablePath, static_cast<DWORD>(std::size(executablePath)));
        if (!length || length >= std::size(executablePath)) {
            std::fputs("ExamplePlugin: game executable path unavailable\n", stderr);
            return;
        }
        const auto modulePath = std::filesystem::path(executablePath).parent_path()
            / L"reframework" / L"plugins" / L"UserDataExtension.dll";
        const auto module = LoadLibraryExW(modulePath.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) {
            std::fprintf(stderr, "ExamplePlugin: UDE load failed (%lu)\n", GetLastError());
            return;
        }
        // Retain this loader reference for the process lifetime.
        using FnRegisterJson = decltype(&RegisterUserDataExtensionJson);
        const auto registerJson = reinterpret_cast<FnRegisterJson>(
            GetProcAddress(module, "RegisterUserDataExtensionJson"));
        if (!registerJson) {
            std::fputs("ExamplePlugin: UDE JSON export missing\n", stderr);
            return;
        }
        const char* declaration = R"json({
            "format": "UserDataExtension/v1",
            "extensions": [{
                "target": "GameDesign/Player/ActionData/Common/GlobalParam/PlayerGlobalParam.user",
                "patches": [{
                    "app.user_data.PlayerGlobalParam": {
                        "_GestureWaterGunMaxAmmo": {"op": "set", "value": 255},
                        "_GestureWaterGun_Range": {"op": "set", "value": 80.0}
                    }
                }]
            }]
        })json";
        if (!registerJson("ExamplePlugin/WaterGun", declaration)) {
            std::fputs("ExamplePlugin: UDE declaration rejected; check UDE log\n", stderr);
            return;
        }
        std::fputs("ExamplePlugin: UDE declaration registered\n", stderr);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ExamplePlugin: startup failed: %s\n", error.what());
    } catch (...) {
        std::fputs("ExamplePlugin: startup failed\n", stderr);
    }
}
```

`GetModuleFileNameW(nullptr, ...)` locates the game EXE, not the provider DLL. MO2 resolves the relative installation through the game view. Keep the loader reference for the process lifetime; do not unload UDE after the callback. `decltype` uses the public header's exact export signature without directly calling the imported symbol, so it adds no static UDE PE import. Replace stderr output with your project's logger as appropriate.

JSON-only mods do not need this DLL. Do not also install the same example JSON and register it twice. Generators can supply their own JSON text through the same API with an extension-specific, stable source ID.

### 10.4 Internal Declarations and JSON Compatibility

For UDE implementation and test contributors, the current internal model is:

```text
ConfigurationDeclaration.resourcePatches
  → ResourcePatch { resourceUri, typePatches }
    → TypePatch { rootTypeName, rootType, fieldEdits }
      → FieldEdit { fieldPath, fieldPathSteps, kind, value, items,
                    identityKey, identityKeySteps }
```

JSON v1 keys **do not change**: `extensions/target` map to resource declarations, `patches` to type batches, and each field's `op/value/items/key` to a flat `FieldEdit`. `identityKey` identifies appended elements for deduplication, not the target field path. There is no extra `Operation` wrapper or old-type alias.

Path steps store syntax during parsing. `ValidateTypePatch` validates metadata, while actual field addresses are resolved against each decoded object's concrete type. `PatchExecutor::ApplyTypePatch` locates and prepares a batch before committing it; the rename preserves failure and reference-ownership behavior and adds no global field-layout cache.

Implementation entries: [Plugin.cpp](plugins/UserDataExtension/Plugin.cpp), [configuration/registration](plugins/UserDataExtension/UserDataConfiguration.cpp), [native decode dispatch](plugins/UserDataExtension/UserDataLoading.cpp), and [batch executor](plugins/UserDataExtension/UserDataPatchExecutor.cpp). Use matching CI **SDK ABI3**; update all providers in an instance together with its host, not UDE alone. See the [CI guide](https://github.com/kassent/REClassInjector/blob/main/README.md#10-initialization-and-the-provider-sdk-api) for ABI and ownership contracts.

## 11. Troubleshooting, Updates, and Distribution

### 11.1 Troubleshooting Order

1. Check installation paths and matching versions for `hid.dll`, `UserDataExtension.dll`, and its TOML.
2. Check that JSON files are in direct child group folders, with correct extensions, encoding, and syntax.
3. Inspect sources, ordering, registration, and type/field errors in `logs/UserDataExtension.log`. With a mod manager, follow its log-redirection rules.
4. Verify the `target`, actual root type, field names, selector identities, and value ranges.
5. Confirm that the game loads the target USER, then inspect operation logs and actual results.
6. Check whether another mod uses the same JSON path, record identity, field, or a complete replacement of the target USER.

| Log fragment | Meaning |
|---|---|
| `Registered` | A configuration source has been collected |
| `configuration registration window has closed` | Submission was too late; submit synchronously in the caller's `ci_on_initialize` |
| `Order=` | Final source execution order |
| `rejected source=` | Type/operation validation failed; inspect the reason |
| `Applied` | A field operation executed; appends report added/skipped counts |
| `Conflict append` | Overlapping entry identity; the existing entry is retained |
| `Conflict set` | Overlapping source writes; the later value applies |
| `stopped USER=` | Further operations on this root stopped; inspect the failed patch and field |

### 11.2 After a Game Update

Check that paths and types still exist, field types and meanings remain compatible, selectors remain unique, and built-in additions are preserved. Pay particular attention to physical-index selectors and configurations using `set` on collections.

### 11.3 Distribution and Removal

Distribute JSON in its own group, required additional resources, and documentation covering runtime dependencies, supported game versions, targets, and conflicts. Manage the shared UDE runtime separately from each mod's configuration.

Before release, check the mod by itself, coexistence with other mods, target resource reloads, scene changes, and normal exit. To uninstall, remove the mod's configuration and owned files, then restart; retain shared runtimes required by other mods. Ordinary UDE memory changes are not written back to USER files, but persistent effects triggered through game systems, such as saving, still follow those systems' rules.

## 12. Frequently Asked Questions

### Does UDE Eliminate All Mod Conflicts?

UDE reduces whole-file replacement conflicts and lets independent operations coexist. Competing field writes, shared record identities, same-path JSON files, and mutually exclusive game rules still require coordination.

### Will Configuration Always Survive Game Updates Unchanged?

Configuration applies to currently loaded data, making it easier to preserve game updates. Changes to resources, types, fields, IDs, or rules still require adjustments and testing.

### Do I Need to Extract and Repack the USER?

For supported changes to an existing USER, distribute JSON operations rather than regenerating or packaging that target USER. Understanding fields may require public databases, type information, or inspection tools. New private resources still need normal authoring.

### Can I Copy a Public Database JSON Directly into the Configuration Folder?

Database exports provide structure and reference data. Extract the desired differences and wrap them in UDE's operation format before installation; see Section 9.

### Can `append` Update an Existing Row with the Same ID?

`append` preserves entries with existing identities. To edit a field, use a unique selector with `set` or arithmetic. To append and then adjust, use successive patches.

### Why Did Editing JSON Have No Effect?

Fully restart and enter a feature that loads the target USER. Then use logs to check directories, target paths, types, fields, and results. Reloading Lua or editing a file alone does not recollect UDE configuration.

### Can UDE Define New Types or Generate Gameplay Directly?

ClassInjector registers types, while components or game-system code implement behavior. UDE constructs supported data, requests resource references, and modifies existing USERs. New entries still need their corresponding native consumers and game-system integration.

---

**Practical essentials: use current native data as the baseline, express intent as minimal JSON differences, give each mod distinct sources and stable identities, and check final results through the actual game systems.**
