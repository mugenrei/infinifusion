# JSON mod format

## Manifest

Every mod starts with a manifest that tells Infinifusion which mod it is and which JSON patches to load, for example:

```json
{
  "schema_version": 1,
  "id": "example.parameters",
  "name": "Example parameters",
  "version": "1.0",
  "enabled": true,
  "priority": 0,
  "patches": ["table.json", "message.json"]
}
```

You need `schema_version`, an `id` that isn't empty and `patches`, everything else can be left out since `enabled` defaults to `true`, `priority` to `0` and `name` to the mod ID, just make sure your IDs are unique because Infinifusion won't admit two enabled mods with the same ID.

Each mod goes in its own folder and the patch paths have to stay inside that folder, disabling the mod or removing its manifest also removes it from the catalog.

## UDataTable rows

You can change existing rows, add new ones, clone one that's already there or remove rows from a `UDataTable`, here's an example with each of those operations:

```json
{
  "schema_version": 1,
  "asset_type": "data_table",
  "table": "/Game/MyMod/Parameters.Parameters",
  "row_struct": "/Script/MyModule.MyRow",
  "rows": [
    {"row": "Existing", "op": "update", "fields": {"Cost": 20}},
    {"row": "New", "op": "upsert", "fields": {
      "Position": {"X": 1, "Y": 2, "Z": 3},
      "Model": {"$type": "soft_object", "value": "/Game/MyMod/Model.Model"}
    }},
    {"row": "Copy", "op": "clone", "from": "Existing", "fields": {"Cost": 5}},
    {"row": "Obsolete", "op": "remove"}
  ]
}
```

You can use FModel or UAssetGUI to find most of the asset paths, row names and properties you'll need.

`asset_type`, the full `table` path and `rows` are required, while `row_struct` is optional and works as a check to make sure you're patching a table with the exact reflected struct you expect, each entry then selects the row by its FName and uses `update` if you don't specify an `op`.

For the operations, `update` changes a row that already exists and is loaded, `upsert` will update it if it exists or create a new engine-initialized row if it doesn't, `clone` copies the loaded row specified in `from` before applying your changes, and `remove` removes the row, even if it was already absent. Keep in mind that `clone` uses the source row and won't copy a projected row from another mod.

For `fields`, use the actual reflected property names, including Blueprint-generated suffixes when they're present, and if you're editing something nested like a struct or collection, put the whole value inside its root field instead of trying to use dotted property names.

Ownership works per row operation and per **entire root field**, not individual members inside a struct, so two mods changing different parts of the same root property are still touching the same owned field. Infinifusion checks number widths, reflected members, static array lengths and hard references before publishing anything, while Unreal handles allocating, copying and cleaning up managed values.

You can use numbers, booleans, strings, structs and arrays directly, or use a `$type` wrapper when you need something more specific, with support for `name`, `string`, `enum`, `text`, `localized_text`, `object`, `class`, `soft_object`, `soft_class`, `struct`, `array`, `set` and `map`, maps use key/value pairs and `null` references mean `None`.

The DataTable services documentation has the exact syntax for managed values, along with the checks and publication limits.

## ScriptMessage definitions

ScriptMessage patches work for things like character names and other text entries, using the asset path and the original message label to find what you want to change:

```json
{
  "schema_version": 1,
  "asset_type": "script_message",
  "asset": "/Game/Blueprints/Gamedata/BinTable/Common/CharacterName.CharacterName",
  "definitions": [{
    "label": "CHR_NAME_225_NEKOSHOGUN",
    "op": "update",
    "fields": {"MessageTextPages": ["Mugen"]}
  }]
}
```

Use the full asset path and the source `label`, which stays as the selector even if you change `MessageLabel` in the fields, so you're still targeting the original entry rather than looking it up by the new label.

The operations are similar to DataTables, `update` needs an existing entry, `upsert` updates one or initializes it if it's missing, `clone` copies the source label specified in `from`, and `remove` removes the entry. The changes go through the reflected `Messages` array, with the same ownership rules for operations and fields.

If you change `MessageTextPages`, you're replacing the **entire array**, not just one page, and the engine handles the FText values including longer strings. Final message labels also have to be unique FNames, otherwise publication gets rejected.

Adding a new entry to the message array doesn't necessarily mean the game will register or use it in a dialogue flow, those are separate steps, and the message services documentation covers the limits and which behaviors have actually been verified.

Also, localized assets such as `/Game/L10N/en/...` and their regular counterparts are separate sources, so patch whichever one the game actually loads, it won't automatically apply the same changes to every language.

## Object and Blueprint properties

This lets you change reflected properties on objects, including Blueprint-related ones, for example:

```json
{
  "schema_version": 1,
  "asset_type": "object",
  "object": "/Game/Blueprints/Character/Devil/Dev225/Dev225.Dev225_C.CharaParam_GEN_VARIABLE",
  "class": "/Script/Project.CharaParamComponent",
  "restore_policy": "none",
  "edits": [{"property": "m_NkmScale", "value": 1.75}]
}
```

You can use `blueprint_properties` instead of `object` for `asset_type` since they're aliases, and `class` is optional if you want Infinifusion to check that the selected target has the exact reflected class you're expecting.

Property paths can go through structs using dots or through **existing** array elements with `[n]`, scalar properties support booleans, width-checked signed and unsigned integers, finite floats and enums by value or unique label. Generated property prefixes can be resolved when there's only one match, but I'd use the exact captured name whenever possible.

Strings, FText, references, structs and complete containers use the same reflected-property handling, so the `$type` wrappers from the DataTable section work here too.

Normally the `object` path selects the target object, but you can also use `selector: "defaults"` for an existing class default object or `selector: "component:CharaParam"` for an effective component template, the `class` check applies to whichever one gets selected and none of these options will create a missing Blueprint class or default object.

For managed or nested edits you're always changing the whole root property, so group the members of the same struct in one value instead of trying to edit them independently, and you can use `expect` when you want to check the original value before applying your change.

`restore_policy` defaults to `required`, or you can set it to `none` if you don't want that edit restored, although restoration still depends on verified object identity, lifetime and the last value Infinifusion published.

For `drift_policy`, the default is `preserve_external_change`, which revisits the configuration when the patch intent or target changes, while `persistent_overlay` keeps checking and enforcing the patch continuously at a higher runtime cost.

One thing to keep in mind is that changing a Blueprint template won't rewrite every object that was already created or every cached construction path, new instances will only get those values through the lifecycle paths that the game supports.

## TBCR records

TBCR is used for the records inside supported `MMIAsset` sources, these aren't UDataTable rows and the patch works on the underlying record data instead:

```json
{
  "schema_version": 1,
  "asset_type": "tbcr",
  "asset": "/Game/Blueprints/Gamedata/BinTable/Skill/SkillData.SkillData",
  "edits": [{
    "id": "skill-mp",
    "op": "update",
    "table": 0,
    "record_size": 196,
    "entry": {"index": 30},
    "writes": [{"offset": 40, "type": "i16", "value": 20}]
  }]
}
```

You select the exact asset, table and record, then make your writes using offsets relative to that record rather than process memory addresses, you can also use supported aliases or named properties and both methods go through the same validation, ownership and publication handling. You don't have to rebuild the container framing yourself, and process addresses aren't accepted here.

`update` uses a stable edit `id` and an entry index from the base snapshot starting at `0`, while `append` uses its own stable `id` without an entry selector and has to initialize the entire new record. For raw writes you specify `offset`, `type` and `record_size`, while named writes use a property and its value.

Appended entries are identified by the owning mod and edit ID, not their `SortID` or contents, so if two mods independently append the same record they'll still be separate entries, with the original records coming before any appended ones.

The TBCR services documentation covers supported types, aliases, structural checks, preserved framing, engine allocation and which game consumers have been tested.

## Conflicts, reloads and what actually updates

Infinifusion parses the JSON on a worker and publishes a copied generation of the full mod catalog, but there are still different things happening along the way, from accepting a mod into the catalog to resolving who owns each change, publishing it to the source and finally having the game actually use it, so **a patch being accepted doesn't automatically mean you'll see the result in-game**.

Priority and mod order are used when resolving conflicts, matching values from different mods can coexist, while different values targeting the same thing will produce diagnostics showing the conflict and which mod won. For DataTables and messages the ownership is for the whole root field rather than individual nested values, TBCR updates use typed write ranges and appends have their own identities.

For tables, messages and TBCR, `restore_policy` can be omitted or set to `required`, withdrawing a patch removes its logical definitions but doesn't automatically undo data that's already been materialized into an object or cached by gameplay. Certified `ConditionalPostLoad` paths apply the current definitions through their corresponding type handler, and the fresh Unite parser also uses the TBCR publisher for its verified cached-source recomposition, although that's specific to that consumer path.

### Reloading

Press **F6** to publish the latest JSON changes, even when `HotReload=false`, and automatic save detection uses `PollIntervalMs` with repeated stable reads so it doesn't try to load a file in the middle of being saved.

If a patch has invalid JSON, paths, schema or fails catalog admission, Infinifusion keeps the last catalog that loaded successfully rather than replacing it with broken data, and newly loaded sources will consume the current definitions. Anything the game only loads at startup will still need a restart, since F6 doesn't rebuild menus, tables or widgets that are already in memory, and a successful publication doesn't guarantee that the game refreshed what you're looking at.

### Limits

You can check `infinifusion.log` to see what happened during loading and publication, including source/request/outcome diagnostics, TABLE/TBCR copied publication results and `API-PERF` information.

## Troubleshooting

If a mod isn't working, set `[Logging] DebugMode=true`, restart the game and try reproducing the issue, it'll include more source-publication details and API timing summaries in `infinifusion.log`, while normal logging already includes startup information, JSON admission results, warnings and errors.
