#!/usr/bin/env python3
"""Validate the stable inspection contract using only the Python standard library."""

import argparse
import base64
import binascii
import json
import re
from pathlib import Path


SEMANTIC = re.compile(r"^sem:1:[0-9a-f]{32}$")
IDS = re.compile(
    r"^(?:declaration|expr|local|syntax-type|type-parameter|effect-parameter|"
    r"type-app|signature|capability-member|variant|field|pattern|match-arm|trait-method|provenance|"
    r"obligation|snapshot):[0-9]+$"
)
TYPE_SNAPSHOTS = {
    "application": "type-app",
    "function_signature": "signature",
    "capability_operation": "capability-member",
    "variant": "variant",
    "parameter": "type-parameter",
    "trait_method": "trait-method",
}
SEMANTIC_TYPES = {"nominal", "function", "self"}
PRIMITIVE_TYPES = {"unknown", "error", "int64", "bool", "text", "unit", "never"}
ITEM_KINDS = {"record", "enum", "type", "capability", "function", "trait", "implementation", "test"}
EXPRESSION_KINDS = {"error", "integer", "string", "bool", "unit", "path", "unary", "binary", "call", "field", "tuple", "record", "if", "match", "block", "propagate", "handle", "result", "old", "type_application"}
OCCURRENCE_KINDS = {"declaration", "import", "expression", "type", "trait", "bound"}
ARTIFACT_TAGS = {
    "syntax": "sol.inspection.syntax",
    "hir": "sol.inspection.hir",
    "types": "sol.inspection.types",
    "effects": "sol.inspection.effects",
    "contracts": "sol.inspection.contracts",
    "diagnostics": "sol.inspection.diagnostics",
}
ARTIFACT_VERSIONS = {"syntax": 3, "types": 3, "hir": 1, "effects": 1,
                     "contracts": 1, "diagnostics": 1}


def require_object(value, fields, where):
    assert isinstance(value, dict), f"{where}: expected object"
    missing = set(fields) - set(value)
    assert not missing, f"{where}: missing {sorted(missing)}"


def walk(value):
    yield value
    if isinstance(value, dict):
        for child in value.values():
            yield from walk(child)
    elif isinstance(value, list):
        for child in value:
            yield from walk(child)


def schema_accepts(schema, definition, value):
    if "$ref" in definition:
        return schema_accepts(schema, schema["$defs"][definition["$ref"].split("/")[-1]], value)
    if "anyOf" in definition:
        if not any(schema_accepts(schema, branch, value) for branch in definition["anyOf"]):
            return False
    if "oneOf" in definition:
        if sum(schema_accepts(schema, branch, value) for branch in definition["oneOf"]) != 1:
            return False
    if "not" in definition and schema_accepts(schema, definition["not"], value):
        return False
    if "const" in definition and value != definition["const"]:
        return False
    if "enum" in definition and value not in definition["enum"]:
        return False
    kinds = definition.get("type")
    if kinds is not None:
        kinds = [kinds] if isinstance(kinds, str) else kinds
        matches = {"object": isinstance(value, dict), "array": isinstance(value, list),
                   "string": isinstance(value, str), "integer": type(value) is int,
                   "boolean": isinstance(value, bool), "null": value is None}
        if not any(matches.get(kind, False) for kind in kinds):
            return False
    if isinstance(value, dict):
        if not set(definition.get("required", ())) <= set(value):
            return False
        for key, child in definition.get("properties", {}).items():
            if key in value and not schema_accepts(schema, child, value[key]):
                return False
    if isinstance(value, list):
        if len(value) < definition.get("minItems", 0):
            return False
        if "maxItems" in definition and len(value) > definition["maxItems"]:
            return False
        if "items" in definition and not all(
                schema_accepts(schema, definition["items"], child) for child in value):
            return False
    if isinstance(value, str):
        if len(value) < definition.get("minLength", 0):
            return False
        if "pattern" in definition and re.fullmatch(definition["pattern"], value) is None:
            return False
    if type(value) is int:
        if value < definition.get("minimum", value):
            return False
        if value > definition.get("maximum", value):
            return False
    for condition in definition.get("allOf", ()):
        if "if" not in condition or schema_accepts(schema, condition["if"], value):
            if not schema_accepts(schema, condition.get("then", condition), value):
                return False
        elif "else" in condition and not schema_accepts(schema, condition["else"], value):
            return False
    return True


def validate_schema(schema, previous):
    require_object(schema, ["$schema", "$id", "required", "properties", "$defs"], "schema")
    assert schema["properties"]["schema"] == {"const": "sol.inspection"}
    assert schema["properties"]["version"] == {"const": 3}
    for name, tag in ARTIFACT_TAGS.items():
        artifact = schema["$defs"][name]
        assert tag == artifact["properties"]["schema"]["const"]
        assert artifact["properties"]["version"] == {"const": ARTIFACT_VERSIONS[name]}
        assert "schema" in artifact["required"] and "version" in artifact["required"]
        missing = set(artifact["required"]) - set(artifact["properties"])
        assert not missing, f"schema {name} required fields lack definitions: {sorted(missing)}"
    type_schema = schema["$defs"]["types"]
    for field in type_schema["required"]:
        definition = type_schema["properties"][field]
        assert definition, f"schema types.{field} has an empty definition"
        assert any(key in definition for key in ("type", "$ref", "allOf", "anyOf", "const")), (
            f"schema types.{field} lacks a meaningful constraint"
        )
    for name in (
        "semanticId", "exprId", "obligationId", "snapshotId", "span", "type",
        "nullableType", "contractObligation", "contractSnapshot", "expressionSnapshot",
        "position", "byteSpan", "diagnosticLocation", "diagnostic", "atom", "effectRow",
        "diagnostics", "patternId", "matchArmId", "fieldId", "variantId",
    ):
        assert name in schema["$defs"], f"schema missing definition {name}"
    contracts = schema["$defs"]["contracts"]["properties"]
    assert contracts["obligations"]["items"] == {"$ref": "#/$defs/contractObligation"}
    assert contracts["snapshots"]["items"] == {"$ref": "#/$defs/contractSnapshot"}
    assert contracts["expressionSnapshots"]["items"] == {"$ref": "#/$defs/expressionSnapshot"}
    obligation = schema["$defs"]["contractObligation"]["properties"]
    assert obligation["id"] == {"$ref": "#/$defs/obligationId"}
    assert obligation["kind"] == {"enum": ["requires", "ensures"]}
    assert obligation["outcome"] == {"enum": ["always", "success", "failure"]}
    assert obligation["predicate"] == {"$ref": "#/$defs/exprId"}
    assert obligation["predicateType"] == {"$ref": "#/$defs/type"}
    assert obligation["resultType"] == {"$ref": "#/$defs/nullableType"}
    assert obligation["snapshotStart"] == {"$ref": "#/$defs/nonnegative"}
    assert obligation["snapshotCount"] == {"$ref": "#/$defs/nonnegative"}
    snapshot = schema["$defs"]["contractSnapshot"]["properties"]
    assert snapshot["id"] == {"$ref": "#/$defs/snapshotId"}
    assert snapshot["obligation"] == {"$ref": "#/$defs/obligationId"}
    assert snapshot["oldExpression"] == snapshot["operand"] == {"$ref": "#/$defs/exprId"}
    assert snapshot["type"] == {"$ref": "#/$defs/type"}
    expression_snapshot = schema["$defs"]["expressionSnapshot"]["properties"]
    assert expression_snapshot["expression"] == {"$ref": "#/$defs/exprId"}
    assert expression_snapshot["snapshot"] == {"$ref": "#/$defs/snapshotId"}
    assert schema["$defs"]["diagnostics"]["properties"]["items"]["items"] == {
        "$ref": "#/$defs/diagnostic"
    }
    locations = schema["$defs"]["diagnostic"]["properties"]["locations"]
    assert locations["minItems"] == 1
    assert locations["items"] == {"$ref": "#/$defs/diagnosticLocation"}
    location = schema["$defs"]["diagnosticLocation"]
    assert set(location["required"]) == {"file", "start", "end", "role", "byteSpan"}
    assert location["properties"]["role"] == {"const": "primary"}
    assert location["properties"]["file"] == {"$ref": "#/$defs/path"}
    assert location["properties"]["start"] == location["properties"]["end"] == {
        "$ref": "#/$defs/position"
    }
    assert location["properties"]["byteSpan"] == {"$ref": "#/$defs/byteSpan"}
    position = schema["$defs"]["position"]["properties"]
    assert position["line"] == position["column"] == {"$ref": "#/$defs/positiveInteger"}
    byte_span = schema["$defs"]["byteSpan"]["properties"]
    assert byte_span["start"] == byte_span["end"] == {"$ref": "#/$defs/nonnegative"}
    assert "tuple" in schema["$defs"]["syntax"]["properties"]["expressions"]["items"]["properties"]["kind"]["enum"]
    assert schema["$defs"]["types"]["properties"]["applications"]["items"] == {
        "$ref": "#/$defs/typeApplication"
    }
    applications = schema["$defs"]["typeApplication"]["properties"]
    assert "tuple" in applications["constructor"]["enum"]
    assert schema["$defs"]["syntax"]["properties"]["patterns"]["items"] == {
        "$ref": "#/$defs/pattern"
    }
    assert schema["$defs"]["syntax"]["properties"]["matchArms"]["items"] == {
        "$ref": "#/$defs/matchArm"
    }
    assert schema["$defs"]["types"]["properties"]["patterns"]["items"] == {
        "$ref": "#/$defs/patternType"
    }
    assert schema["$defs"]["types"]["properties"]["variants"]["items"] == {
        "$ref": "#/$defs/variantSemantic"
    }
    assert schema["$defs"]["types"]["properties"]["fields"]["items"] == {
        "$ref": "#/$defs/fieldSemantic"
    }
    unchanged = set(previous["$defs"]) - {"syntax", "types"}
    for name in unchanged:
        assert schema["$defs"][name] == previous["$defs"][name], (
            f"v3 weakened or changed unchanged v2 definition {name}"
        )
    for name in ("files", "declarations", "arenaCounts"):
        assert schema["$defs"]["syntax"]["properties"][name] == previous["$defs"]["syntax"]["properties"][name]
    preserved_type_fields = set(previous["$defs"]["types"]["properties"]) - {
        "schema", "version", "patternVariantResolutions"
    }
    for name in preserved_type_fields:
        assert schema["$defs"]["types"]["properties"][name] == previous["$defs"]["types"]["properties"][name], (
            f"v3 weakened unchanged types.{name}"
        )

    semantic = "sem:1:" + "0" * 32
    primitive = {"kind": "int64", "definition": None, "snapshotRef": None}
    span = {"file": "sample.sol", "start": 0, "end": 1}
    hir_definition = {"semanticId": semantic, "kind": "record", "name": "R",
                      "syntaxDeclaration": "declaration:0"}
    assert schema_accepts(schema, schema["$defs"]["hir"]["properties"]["definitions"]["items"], hir_definition)
    assert not schema_accepts(schema, schema["$defs"]["hir"]["properties"]["definitions"]["items"],
                              {key: value for key, value in hir_definition.items() if key != "name"})
    signature = {"id": "signature:0", "parameters": [primitive], "accesses": ["owned"],
                 "result": primitive, "effects": [], "rowTail": None}
    assert schema_accepts(schema, schema["$defs"]["types"]["properties"]["functionSignatures"]["items"], signature)
    assert not schema_accepts(schema, schema["$defs"]["types"]["properties"]["functionSignatures"]["items"],
                              {key: value for key, value in signature.items() if key != "result"})
    call = {"call": "expr:0", "function": semantic, "arguments": []}
    assert schema_accepts(schema, schema["$defs"]["types"]["properties"]["callInstantiations"]["items"], call)
    assert not schema_accepts(schema, schema["$defs"]["types"]["properties"]["callInstantiations"]["items"],
                              {"call": "expr:0", "function": semantic})
    tuple_type = {"id": "type-app:0", "constructor": "tuple", "definition": None,
                  "arguments": [primitive, primitive]}
    assert schema_accepts(schema, schema["$defs"]["typeApplication"], tuple_type)
    tuple_type["arguments"] = [primitive]
    assert not schema_accepts(schema, schema["$defs"]["typeApplication"], tuple_type)
    field_expression = {"id": "expr:0", "kind": "field", "span": span, "base": "expr:1"}
    expression_schema = schema["$defs"]["syntax"]["properties"]["expressions"]["items"]
    assert schema_accepts(schema, expression_schema, field_expression)
    del field_expression["base"]
    assert not schema_accepts(schema, expression_schema, field_expression)
    plain_expression = {"id": "expr:0", "kind": "integer", "span": span}
    for extra in ({"base": "expr:1"}, {"scrutinee": "expr:1"}, {"arms": []}):
        assert not schema_accepts(schema, expression_schema, plain_expression | extra)
    match_expression = plain_expression | {
        "kind": "match", "scrutinee": "expr:1", "arms": []
    }
    assert schema_accepts(schema, expression_schema, match_expression)
    assert not schema_accepts(schema, expression_schema,
                              match_expression | {"base": "expr:2"})

    pattern_schema = schema["$defs"]["pattern"]
    child = {"pattern": "pattern:1", "field": None}
    pattern = {"id": "pattern:0", "kind": "tuple", "span": span, "name": None,
               "boolValue": None, "children": [child, child]}
    assert schema_accepts(schema, pattern_schema, pattern)
    pattern["children"][0] = child | {"field": "value"}
    assert not schema_accepts(schema, pattern_schema, pattern)
    pattern |= {"kind": "variant", "name": "item", "children": [child]}
    assert schema_accepts(schema, pattern_schema, pattern)
    pattern["children"] = [child | {"field": "value"}]
    assert not schema_accepts(schema, pattern_schema, pattern)
    pattern |= {"kind": "record", "name": "Box"}
    assert schema_accepts(schema, pattern_schema, pattern)
    pattern["children"] = [child]
    assert not schema_accepts(schema, pattern_schema, pattern)

    pattern_type_child = schema["$defs"]["patternTypeChild"]
    typed_child = {"pattern": "pattern:1", "type": primitive,
                   "field": "field:0", "tupleOrdinal": None}
    assert schema_accepts(schema, pattern_type_child, typed_child)
    assert schema_accepts(schema, pattern_type_child,
                          typed_child | {"field": None, "tupleOrdinal": 0})
    assert not schema_accepts(schema, pattern_type_child,
                              typed_child | {"tupleOrdinal": 0})
    assert not schema_accepts(schema, pattern_type_child,
                              typed_child | {"field": None})
    for node in walk(schema):
        if isinstance(node, dict) and isinstance(node.get("$ref"), str):
            reference = node["$ref"]
            assert reference.startswith("#/$defs/"), f"schema has external reference {reference!r}"
            assert reference.removeprefix("#/$defs/") in schema["$defs"], (
                f"schema has unresolved reference {reference!r}"
            )


def validate_type(value, semantic_ids, snapshot_ids):
    require_object(value, ["kind", "definition", "snapshotRef"], "type")
    kind = value["kind"]
    definition = value["definition"]
    snapshot = value["snapshotRef"]
    if kind in SEMANTIC_TYPES:
        assert SEMANTIC.fullmatch(definition or ""), f"{kind}: invalid semantic definition"
        assert definition in semantic_ids, f"{kind}: unknown semantic definition"
        assert snapshot is None, f"{kind}: unexpected snapshotRef"
    elif kind in TYPE_SNAPSHOTS:
        assert definition is None, f"{kind}: semantic definition leaks identity domain"
        assert isinstance(snapshot, str) and snapshot.startswith(TYPE_SNAPSHOTS[kind] + ":")
        assert IDS.fullmatch(snapshot), f"{kind}: invalid snapshotRef"
        assert snapshot in snapshot_ids, f"{kind}: unknown snapshotRef {snapshot!r}"
    else:
        assert kind in PRIMITIVE_TYPES, f"unknown type kind {kind!r}"
        assert definition is None and snapshot is None, f"{kind}: unexpected identity"


def nominal_owner(type_value, applications):
    owner = type_value.get("definition")
    application = applications.get(type_value.get("snapshotRef"))
    if application is not None and application["constructor"] == "user":
        owner = application["definition"]
    return owner


def variant_target_matches(semantic, syntax_pattern, target, applications):
    return target is not None and target["name"] == syntax_pattern["name"] \
        and target["owner"] == nominal_owner(semantic["type"], applications)


def type_structure(value, applications, substitutions=None):
    substitutions = substitutions or {}
    if value["kind"] == "parameter" and value["snapshotRef"] in substitutions:
        return type_structure(substitutions[value["snapshotRef"]], applications,
                              substitutions)
    if value["kind"] == "application":
        application = applications[value["snapshotRef"]]
        return ("application", application["constructor"], application["definition"],
                tuple(type_structure(argument, applications, substitutions)
                      for argument in application["arguments"]))
    return value["kind"], value["definition"], value["snapshotRef"]


def instantiated_field_type_matches(semantic, target, child_type, applications):
    substitutions = {}
    owner_application = applications.get(semantic["type"].get("snapshotRef"))
    if owner_application is not None and owner_application["constructor"] == "user":
        parameters = target["ownerTypeParameters"]
        if len(parameters) != len(owner_application["arguments"]):
            return False
        substitutions = dict(zip(parameters, owner_application["arguments"]))
    return type_structure(target["type"], applications, substitutions) \
        == type_structure(child_type, applications)


def field_target_matches(semantic, syntax_pattern, syntax_child, ordinal, target,
                         applications):
    if target is None or not instantiated_field_type_matches(
            semantic, target, semantic["children"][ordinal]["type"], applications):
        return False
    if syntax_pattern["kind"] == "record":
        return target["parentKind"] == "record" \
            and target["parent"] == nominal_owner(semantic["type"], applications) \
            and target["name"] == syntax_child["field"]
    return target["parentKind"] == "variant" \
        and target["parent"] == semantic["variant"] and target["ordinal"] == ordinal


def validate(path, schema_path, require_generic, require_tuple, require_patterns):
    text = path.read_text(encoding="utf-8")
    assert "18446744073709551615" not in text, "64-bit SIZE_MAX leaked"
    assert "4294967295" not in text, "32-bit SIZE_MAX leaked"
    assert "SIZE_MAX" not in text and "SOL_AST_NONE" not in text, "native sentinel leaked"
    data = json.loads(text)
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    previous = json.loads(schema_path.with_name("sol-inspection-2.schema.json").read_text(encoding="utf-8"))
    validate_schema(schema, previous)

    require_object(data, ["schema", "version", "producer", "package", "artifacts"], "root")
    assert data["schema"] == "sol.inspection" and data["version"] == 3
    require_object(data["producer"], ["name", "version"], "producer")
    assert data["producer"]["name"] == "sol"
    require_object(data["package"], ["kind", "edition", "fileCount"], "package")
    assert data["package"]["kind"] in ("file", "directory")
    assert isinstance(data["package"]["edition"], int) and data["package"]["edition"] >= 0

    artifacts = data["artifacts"]
    require_object(artifacts, ARTIFACT_TAGS, "artifacts")
    required_arrays = {
        "syntax": ["files", "declarations", "expressions", "patterns", "matchArms"],
        "hir": ["definitions", "locals", "expressionResolutions", "typeResolutions", "effectResolutions", "occurrences"],
        "types": ["expressions", "locals", "definitions", "declaredSyntaxTypes", "applications", "functionSignatures", "provenanceRoots", "callInstantiations", "coercions", "handlers", "methodResolutions", "tupleProjections", "representations", "constructions", "variantConstructors", "variants", "fields", "patterns", "argumentFieldResolutions", "implementationTargets"],
        "effects": ["rows", "callInstantiations"],
        "contracts": ["obligations", "snapshots", "expressionSnapshots"],
        "diagnostics": ["items"],
    }
    for name, tag in ARTIFACT_TAGS.items():
        require_object(artifacts[name], ["schema", "version", *required_arrays[name]], name)
        assert artifacts[name]["schema"] == tag
        assert artifacts[name]["version"] == ARTIFACT_VERSIONS[name]
        for field in required_arrays[name]:
            assert isinstance(artifacts[name][field], list), f"{name}.{field}: expected array"

    syntax = artifacts["syntax"]
    files = syntax["files"]
    assert data["package"]["fileCount"] == len(files) and files
    lengths = {}
    sources = {}
    for entry in files:
        require_object(entry, ["path", "byteLength", "sourceBase64", "module"], "syntax.files[]")
        package_path = entry["path"]
        assert isinstance(package_path, str) and package_path
        assert not package_path.startswith(("/", "\\")) and "\\" not in package_path
        assert ".." not in Path(package_path).parts and package_path not in lengths
        try:
            source = base64.b64decode(entry["sourceBase64"], validate=True)
        except (ValueError, binascii.Error) as error:
            raise AssertionError(f"invalid source base64 for {package_path}: {error}") from error
        assert len(source) == entry["byteLength"] >= 0
        lengths[package_path] = len(source)
        sources[package_path] = source

    declarations = syntax["declarations"]
    semantic_ids = {entry["semanticId"] for entry in declarations}
    assert len(semantic_ids) == len(declarations)
    assert all(SEMANTIC.fullmatch(value) for value in semantic_ids)
    assert [entry["id"] for entry in declarations] == [
        f"declaration:{index}" for index in range(len(declarations))
    ]
    assert all(entry["kind"] in ITEM_KINDS for entry in declarations)
    expressions = syntax["expressions"]
    assert [entry["id"] for entry in expressions] == [f"expr:{index}" for index in range(len(expressions))]
    assert all(entry["kind"] in EXPRESSION_KINDS for entry in expressions)
    expression_ids = {entry["id"] for entry in expressions}
    for entry in expressions:
        if entry["kind"] == "field":
            assert entry.get("base") in expression_ids, "field has an invalid base reference"

    patterns = syntax["patterns"]
    arms = syntax["matchArms"]
    pattern_ids = {entry["id"] for entry in patterns}
    arm_ids = {entry["id"] for entry in arms}
    assert [entry["id"] for entry in patterns] == [
        f"pattern:{index}" for index in range(len(patterns))
    ]
    assert [entry["id"] for entry in arms] == [
        f"match-arm:{index}" for index in range(len(arms))
    ]
    assert len(pattern_ids) == len(patterns) and len(arm_ids) == len(arms)
    pattern_by_id = {entry["id"]: entry for entry in patterns}
    owners = {pattern_id: 0 for pattern_id in pattern_ids}
    arm_owners = {arm_id: 0 for arm_id in arm_ids}
    for pattern in patterns:
        require_object(pattern, ["id", "kind", "span", "name", "boolValue", "children"], "pattern")
        assert pattern["kind"] in {"wildcard", "bool", "binding", "variant", "record", "tuple"}
        named = pattern["kind"] in {"binding", "variant", "record"}
        assert (isinstance(pattern["name"], str) and pattern["name"] != "") if named else pattern["name"] is None
        assert isinstance(pattern["boolValue"], bool) if pattern["kind"] == "bool" else pattern["boolValue"] is None
        has_children = pattern["kind"] in {"variant", "record", "tuple"}
        assert has_children or not pattern["children"]
        if pattern["kind"] == "tuple":
            assert 2 <= len(pattern["children"]) <= 16
        source = sources[pattern["span"]["file"]][pattern["span"]["start"]:pattern["span"]["end"]]
        if named:
            assert pattern["name"].encode("utf-8") in source, "pattern name is not its source spelling"
        for child in pattern["children"]:
            require_object(child, ["pattern", "field"], "pattern child")
            assert child["pattern"] in pattern_ids
            owners[child["pattern"]] += 1
            if pattern["kind"] == "record":
                assert isinstance(child["field"], str) and child["field"]
                assert child["field"].encode("utf-8") in source, "record field is not its source spelling"
            else:
                assert child["field"] is None
    for arm in arms:
        require_object(arm, ["id", "pattern", "guard", "value", "span"], "match arm")
        assert arm["pattern"] in pattern_ids and arm["value"] in expression_ids
        assert arm["guard"] is None or arm["guard"] in expression_ids
        owners[arm["pattern"]] += 1
        root_span = pattern_by_id[arm["pattern"]]["span"]
        value_span = expressions[int(arm["value"].split(":")[1])]["span"]
        assert arm["span"]["file"] == root_span["file"] == value_span["file"]
        assert arm["span"]["start"] == root_span["start"]
        assert arm["span"]["end"] == value_span["end"]
    for expression in expressions:
        if expression["kind"] == "match":
            assert expression.get("scrutinee") in expression_ids
            assert isinstance(expression.get("arms"), list)
            starts = [arms[int(arm_id.split(":")[1])]["span"]["start"]
                      for arm_id in expression["arms"]]
            assert starts == sorted(starts), "match arms are not in source order"
            for arm_id in expression["arms"]:
                assert arm_id in arm_ids
                arm_owners[arm_id] += 1
        else:
            assert "scrutinee" not in expression and "arms" not in expression
    assert all(count == 1 for count in owners.values()), "patterns must have exactly one parent or arm owner"
    assert all(count == 1 for count in arm_owners.values()), "match arms must have exactly one match owner"

    visiting = set()
    visited = set()
    def visit_pattern(pattern_id):
        assert pattern_id not in visiting, "recursive pattern cycle"
        if pattern_id in visited:
            return
        visiting.add(pattern_id)
        for child in pattern_by_id[pattern_id]["children"]:
            visit_pattern(child["pattern"])
        visiting.remove(pattern_id)
        visited.add(pattern_id)
    for pattern_id in pattern_ids:
        visit_pattern(pattern_id)

    hir = artifacts["hir"]
    assert {entry["semanticId"] for entry in hir["definitions"]} == semantic_ids
    assert all(entry["kind"] in ITEM_KINDS for entry in hir["definitions"])
    assert all(entry.get("access") in ("owned", "shared", "exclusive")
               for entry in hir["locals"])
    for occurrence in hir["occurrences"]:
        assert occurrence["kind"] in OCCURRENCE_KINDS
        assert occurrence["target"] in semantic_ids
    for resolution in hir["effectResolutions"]:
        require_object(resolution, ["ownerKind", "ownerIndex", "kind", "snapshotTarget"], "effect resolution")
        assert resolution["kind"] in ("atom", "parameter", "error")
        if resolution["kind"] == "parameter":
            assert re.fullmatch(r"effect-parameter:[0-9]+", resolution["snapshotTarget"] or "")
        else:
            assert resolution["snapshotTarget"] is None, "effect atom/error invented a target"

    types = artifacts["types"]
    snapshot_ids = {
        entry["id"] for field in ("applications", "functionSignatures", "provenanceRoots")
        for entry in types[field]
    }
    snapshot_ids.update(f"capability-member:{index}" for index in range(syntax["arenaCounts"]["capabilityMembers"]))
    snapshot_ids.update(f"variant:{index}" for index in range(len(types["variantConstructors"])))
    snapshot_ids.update(f"type-parameter:{index}" for index in range(syntax["arenaCounts"]["typeParameters"]))
    snapshot_ids.update(f"trait-method:{index}" for index in range(syntax["arenaCounts"]["traitMethods"]))
    for node in walk(data):
        if isinstance(node, dict) and set(("kind", "definition", "snapshotRef")) <= set(node):
            validate_type(node, semantic_ids, snapshot_ids)
        if (isinstance(node, dict) and set(("file", "start", "end")) <= set(node)
                and "byteSpan" not in node):
            assert node["file"] in lengths, f"span references unknown file {node['file']!r}"
            assert isinstance(node["start"], int) and isinstance(node["end"], int)
            assert 0 <= node["start"] <= node["end"] <= lengths[node["file"]]
        if isinstance(node, str) and (node.startswith("sem:") or re.match(r"^[a-z-]+:[0-9]", node)):
            assert SEMANTIC.fullmatch(node) or IDS.fullmatch(node) or node.startswith(("builtin:", "builtin-type:")), f"invalid reference {node!r}"

    applications_by_id = {entry["id"]: entry for entry in types["applications"]}
    assert len(applications_by_id) == len(types["applications"])
    for index, application in enumerate(types["applications"]):
        require_object(application, ["id", "constructor", "definition", "arguments"], "type application")
        assert application["id"] == f"type-app:{index}"
        assert application["constructor"] in ("option", "result", "user", "tuple")
        if application["constructor"] == "tuple":
            assert application["definition"] is None
            assert 2 <= len(application["arguments"]) <= 16
    projections = types["tupleProjections"]
    assert len(projections) == len(expressions)
    expression_types = {entry["subject"]: entry["type"] for entry in types["expressions"]}
    assert len(expression_types) == len(expressions) and set(expression_types) == expression_ids
    for index, ordinal in enumerate(projections):
        assert ordinal is None or type(ordinal) is int and 0 <= ordinal < 16
        expression = expressions[index]
        if expression["kind"] != "field":
            assert ordinal is None, "non-field expression has a tuple projection"
            continue
        base_type = expression_types[expression["base"]]
        application = applications_by_id.get(base_type.get("snapshotRef"))
        tuple_application = application is not None and application["constructor"] == "tuple"
        if tuple_application:
            assert ordinal is not None and ordinal < len(application["arguments"]), (
                "tuple field has a missing or out-of-range projection"
            )
            base = expressions[int(expression["base"].split(":")[1])]
            assert base["span"]["file"] == expression["span"]["file"]
            selector = sources[expression["span"]["file"]][
                base["span"]["end"]:expression["span"]["end"]
            ]
            assert selector == f".{ordinal}".encode("ascii"), (
                "tuple projection does not match its source selector"
            )
            assert expression_types[expression["id"]] == application["arguments"][ordinal], (
                "tuple projection type does not match its selected element"
            )
        else:
            assert ordinal is None, "non-tuple field has a tuple projection"
    for signature in types["functionSignatures"]:
        assert len(signature.get("accesses", [])) == len(signature["parameters"])
        assert all(access in ("owned", "shared", "exclusive")
                   for access in signature["accesses"])
    variants = types["variants"]
    fields = types["fields"]
    assert [entry["id"] for entry in variants] == [
        f"variant:{index}" for index in range(syntax["arenaCounts"]["variants"])
    ]
    assert [entry["id"] for entry in fields] == [
        f"field:{index}" for index in range(syntax["arenaCounts"]["fields"])
    ]
    variant_by_id = {entry["id"]: entry for entry in variants}
    field_by_id = {entry["id"]: entry for entry in fields}
    for variant in variants:
        require_object(variant, ["id", "owner", "name"], "variant semantic")
        assert variant["owner"] in semantic_ids and isinstance(variant["name"], str)
    parent_ordinals = {}
    for field in fields:
        require_object(field, ["id", "parentKind", "parent", "owner", "name", "ordinal",
                               "ownerTypeParameters", "type"],
                       "field semantic")
        assert field["owner"] in semantic_ids and field["parentKind"] in ("record", "variant")
        assert isinstance(field["name"], str) and field["name"]
        assert type(field["ordinal"]) is int and field["ordinal"] >= 0
        assert isinstance(field["ownerTypeParameters"], list)
        assert len(set(field["ownerTypeParameters"])) == len(field["ownerTypeParameters"])
        assert all(re.fullmatch(r"type-parameter:[0-9]+", parameter)
                   and int(parameter.split(":")[1]) < syntax["arenaCounts"]["typeParameters"]
                   for parameter in field["ownerTypeParameters"])
        if field["parentKind"] == "record":
            assert field["parent"] == field["owner"]
        else:
            assert field["parent"] in variant_by_id
            assert variant_by_id[field["parent"]]["owner"] == field["owner"]
        parent_ordinals.setdefault(field["parent"], []).append(field["ordinal"])
    assert all(sorted(ordinals) == list(range(len(ordinals)))
               for ordinals in parent_ordinals.values())
    pattern_types = types["patterns"]
    assert [entry["subject"] for entry in pattern_types] == [
        f"pattern:{index}" for index in range(len(patterns))
    ]
    pattern_semantics = {entry["subject"]: entry for entry in pattern_types}
    assert len(pattern_semantics) == len(pattern_ids)
    for semantic in pattern_types:
        syntax_pattern = pattern_by_id[semantic["subject"]]
        require_object(semantic, ["subject", "type", "variant", "children"], "pattern type")
        assert (semantic["variant"] is not None) == (syntax_pattern["kind"] == "variant")
        if semantic["variant"] is not None:
            assert re.fullmatch(r"variant:[0-9]+", semantic["variant"])
            assert semantic["variant"] in variant_by_id
            resolved_variant = variant_by_id[semantic["variant"]]
            assert variant_target_matches(semantic, syntax_pattern, resolved_variant,
                                          applications_by_id)
        assert len(semantic["children"]) == len(syntax_pattern["children"])
        application = applications_by_id.get(semantic["type"].get("snapshotRef"))
        for ordinal, (child, syntax_child) in enumerate(zip(semantic["children"], syntax_pattern["children"])):
            require_object(child, ["pattern", "type", "field", "tupleOrdinal"], "pattern child type")
            assert child["pattern"] == syntax_child["pattern"]
            assert child["type"] == pattern_semantics[child["pattern"]]["type"]
            if syntax_pattern["kind"] == "tuple":
                assert child["field"] is None and child["tupleOrdinal"] == ordinal
                assert application is not None and application["constructor"] == "tuple"
                assert ordinal < len(application["arguments"])
                assert child["type"] == application["arguments"][ordinal]
            elif syntax_pattern["kind"] in {"record", "variant"}:
                assert re.fullmatch(r"field:[0-9]+", child["field"] or "")
                assert child["field"] in field_by_id
                assert child["tupleOrdinal"] is None
                resolved_field = field_by_id[child["field"]]
                assert field_target_matches(semantic, syntax_pattern, syntax_child,
                                            ordinal, resolved_field, applications_by_id)
            else:
                assert child["field"] is None and child["tupleOrdinal"] is None
    for expression in expressions:
        if expression["kind"] != "match":
            continue
        subject_type = expression_types[expression["scrutinee"]]
        for arm_id in expression["arms"]:
            arm = arms[int(arm_id.split(":")[1])]
            assert pattern_semantics[arm["pattern"]]["type"] == subject_type
            if arm["guard"] is not None:
                assert expression_types[arm["guard"]]["kind"] == "bool", "match guard is not exact Bool"
    for entry in types["representations"]:
        require_object(entry, ["definition", "flavor", "type"], "representation")
        assert entry["definition"] in semantic_ids and entry["flavor"] in ("distinct", "refined")
    for entry in types["implementationTargets"]:
        require_object(entry, ["definition", "target"], "implementation target")
        assert entry["definition"] in semantic_ids

    for call in artifacts["effects"]["callInstantiations"]:
        require_object(call, ["call", "function", "rowTail", "arguments", "instantiatedRow"], "effect call")
        assert re.fullmatch(r"expr:[0-9]+", call["call"]), "absent effect call emitted"
        assert call["function"] in semantic_ids
        assert call["rowTail"] is None or isinstance(call["rowTail"], int)
        assert isinstance(call["arguments"], list) and isinstance(call["instantiatedRow"], list)

    contracts = artifacts["contracts"]
    obligations = contracts["obligations"]
    snapshots = contracts["snapshots"]
    obligation_ids = {entry["id"] for entry in obligations}
    snapshot_ids = {entry["id"] for entry in snapshots}
    assert [entry["id"] for entry in obligations] == [
        f"obligation:{index}" for index in range(len(obligations))
    ]
    assert [entry["id"] for entry in snapshots] == [
        f"snapshot:{index}" for index in range(len(snapshots))
    ]
    for obligation in obligations:
        require_object(obligation, ["id", "kind", "outcome", "predicate", "predicateType", "resultType", "snapshotStart", "snapshotCount"], "contract obligation")
        assert obligation["kind"] in ("requires", "ensures")
        assert obligation["outcome"] in ("always", "success", "failure")
        assert obligation["predicate"] in expression_ids
        assert isinstance(obligation["predicateType"], dict)
        assert obligation["resultType"] is None or isinstance(obligation["resultType"], dict)
        start = obligation["snapshotStart"]
        count = obligation["snapshotCount"]
        assert type(start) is int and type(count) is int and start >= 0 and count >= 0
        assert start <= len(snapshots) and count <= len(snapshots) - start
        expected = obligation["id"]
        assert all(snapshot["obligation"] == expected for snapshot in snapshots[start:start + count])
    for index, snapshot in enumerate(snapshots):
        require_object(snapshot, ["id", "obligation", "oldExpression", "operand", "type"], "contract snapshot")
        assert snapshot["obligation"] in obligation_ids
        assert snapshot["oldExpression"] in expression_ids and snapshot["operand"] in expression_ids
        assert isinstance(snapshot["type"], dict)
        obligation = obligations[int(snapshot["obligation"].split(":")[1])]
        start = obligation["snapshotStart"]
        assert start <= index < start + obligation["snapshotCount"]
    expression_snapshots = contracts["expressionSnapshots"]
    assert len(expression_snapshots) == len(snapshots)
    mapped_expressions = set()
    mapped_snapshots = set()
    for mapping in expression_snapshots:
        require_object(mapping, ["expression", "snapshot"], "expression snapshot")
        assert mapping["expression"] in expression_ids and mapping["snapshot"] in snapshot_ids
        assert mapping["expression"] not in mapped_expressions
        assert mapping["snapshot"] not in mapped_snapshots
        mapped_expressions.add(mapping["expression"])
        mapped_snapshots.add(mapping["snapshot"])
        snapshot = snapshots[int(mapping["snapshot"].split(":")[1])]
        assert snapshot["oldExpression"] == mapping["expression"]

    for diagnostic in artifacts["diagnostics"]["items"]:
        require_object(diagnostic, ["schema", "code", "severity", "message", "locations"], "diagnostic")
        assert diagnostic["schema"] == "sol.diagnostic/1"
        assert diagnostic["severity"] in ("error", "warning") and diagnostic["locations"]
        for location in diagnostic["locations"]:
            require_object(location, ["file", "start", "end", "role", "byteSpan"], "diagnostic location")
            assert location["file"] in lengths
            assert location["role"] == "primary"
            for name in ("start", "end"):
                position = location[name]
                require_object(position, ["line", "column"], f"diagnostic location {name}")
                assert type(position["line"]) is int and position["line"] > 0
                assert type(position["column"]) is int and position["column"] > 0
            span = location["byteSpan"]
            require_object(span, ["start", "end"], "diagnostic byte span")
            assert type(span["start"]) is int and type(span["end"]) is int
            assert 0 <= span["start"] <= span["end"] <= lengths[location["file"]]

    if require_generic:
        parameters = [node for node in walk(types) if isinstance(node, dict) and node.get("kind") == "parameter"]
        assert parameters, "generic fixture did not emit a parameter type"
        assert all(node["definition"] is None and re.fullmatch(r"type-parameter:[0-9]+", node["snapshotRef"]) for node in parameters)
        assert types["applications"] and types["callInstantiations"], "generic fixture lacks exact type artifacts"

    if require_tuple:
        assert any(entry["kind"] == "tuple" for entry in expressions), "tuple syntax kind missing"
        tuples = [entry for entry in types["applications"] if entry["constructor"] == "tuple"]
        assert tuples, "tuple application type missing"
        ordinals = [ordinal for ordinal in projections if ordinal is not None]
        assert ordinals, "tuple projection is missing"

    if require_patterns:
        kinds = {entry["kind"] for entry in patterns}
        assert {"wildcard", "bool", "binding", "variant", "record", "tuple"} <= kinds, (
            "nested-pattern fixture does not cover every recursive pattern kind"
        )
        assert any(entry["guard"] is not None for entry in arms), "guarded match arm is missing"
        assert any(any(pattern_by_id[child["pattern"]]["children"]
                       for child in pattern["children"]) for pattern in patterns), (
            "nested recursive pattern is missing"
        )
        assert any(entry["kind"] == "match" and entry["arms"] == []
                   for entry in expressions), "empty-enum zero-arm match is missing"
        variant_mutation_rejected = False
        field_mutation_rejected = False
        for semantic in pattern_types:
            syntax_pattern = pattern_by_id[semantic["subject"]]
            if semantic["variant"] is not None:
                for candidate in variants:
                    if candidate["id"] != semantic["variant"]:
                        variant_mutation_rejected = variant_mutation_rejected or not \
                            variant_target_matches(semantic, syntax_pattern, candidate,
                                                   applications_by_id)
            for ordinal, (child, syntax_child) in enumerate(
                    zip(semantic["children"], syntax_pattern["children"])):
                if child["field"] is None:
                    continue
                for candidate in fields:
                    if candidate["id"] != child["field"]:
                        field_mutation_rejected = field_mutation_rejected or not \
                            field_target_matches(semantic, syntax_pattern, syntax_child,
                                                 ordinal, candidate, applications_by_id)
        assert variant_mutation_rejected, "malformed variant target was not rejected"
        assert field_mutation_rejected, "malformed field target was not rejected"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("schema", type=Path)
    parser.add_argument("--require-generic", action="store_true")
    parser.add_argument("--require-tuple", action="store_true")
    parser.add_argument("--require-patterns", action="store_true")
    args = parser.parse_args()
    validate(args.output, args.schema, args.require_generic, args.require_tuple,
             args.require_patterns)


if __name__ == "__main__":
    main()
