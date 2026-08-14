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
    r"type-app|signature|capability-member|variant|trait-method|provenance|"
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
EXPRESSION_KINDS = {"error", "integer", "string", "bool", "unit", "path", "unary", "binary", "call", "field", "record", "if", "match", "block", "propagate", "handle", "result", "old", "type_application"}
OCCURRENCE_KINDS = {"declaration", "import", "expression", "type", "trait", "bound"}
ARTIFACT_TAGS = {
    "syntax": "sol.inspection.syntax",
    "hir": "sol.inspection.hir",
    "types": "sol.inspection.types",
    "effects": "sol.inspection.effects",
    "contracts": "sol.inspection.contracts",
    "diagnostics": "sol.inspection.diagnostics",
}


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


def validate_schema(schema):
    require_object(schema, ["$schema", "$id", "required", "properties", "$defs"], "schema")
    assert schema["properties"]["schema"] == {"const": "sol.inspection"}
    assert schema["properties"]["version"] == {"const": 1}
    for name, tag in ARTIFACT_TAGS.items():
        artifact = schema["$defs"][name]
        assert tag == artifact["properties"]["schema"]["const"]
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
        "diagnostics",
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


def validate_type(value, semantic_ids):
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
    else:
        assert kind in PRIMITIVE_TYPES, f"unknown type kind {kind!r}"
        assert definition is None and snapshot is None, f"{kind}: unexpected identity"


def validate(path, schema_path, require_generic):
    text = path.read_text(encoding="utf-8")
    assert "18446744073709551615" not in text, "64-bit SIZE_MAX leaked"
    assert "4294967295" not in text, "32-bit SIZE_MAX leaked"
    assert "SIZE_MAX" not in text and "SOL_AST_NONE" not in text, "native sentinel leaked"
    data = json.loads(text)
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    validate_schema(schema)

    require_object(data, ["schema", "version", "producer", "package", "artifacts"], "root")
    assert data["schema"] == "sol.inspection" and data["version"] == 1
    require_object(data["producer"], ["name", "version"], "producer")
    assert data["producer"]["name"] == "sol"
    require_object(data["package"], ["kind", "edition", "fileCount"], "package")
    assert data["package"]["kind"] in ("file", "directory")
    assert isinstance(data["package"]["edition"], int) and data["package"]["edition"] >= 0

    artifacts = data["artifacts"]
    require_object(artifacts, ARTIFACT_TAGS, "artifacts")
    required_arrays = {
        "syntax": ["files", "declarations", "expressions"],
        "hir": ["definitions", "locals", "expressionResolutions", "typeResolutions", "effectResolutions", "occurrences"],
        "types": ["expressions", "locals", "definitions", "declaredSyntaxTypes", "applications", "functionSignatures", "provenanceRoots", "callInstantiations", "coercions", "handlers", "methodResolutions", "representations", "constructions", "variantConstructors", "patternVariantResolutions", "argumentFieldResolutions", "implementationTargets"],
        "effects": ["rows", "callInstantiations"],
        "contracts": ["obligations", "snapshots", "expressionSnapshots"],
        "diagnostics": ["items"],
    }
    for name, tag in ARTIFACT_TAGS.items():
        require_object(artifacts[name], ["schema", "version", *required_arrays[name]], name)
        assert artifacts[name]["schema"] == tag and artifacts[name]["version"] == 1
        for field in required_arrays[name]:
            assert isinstance(artifacts[name][field], list), f"{name}.{field}: expected array"

    syntax = artifacts["syntax"]
    files = syntax["files"]
    assert data["package"]["fileCount"] == len(files) and files
    lengths = {}
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

    hir = artifacts["hir"]
    assert {entry["semanticId"] for entry in hir["definitions"]} == semantic_ids
    assert all(entry["kind"] in ITEM_KINDS for entry in hir["definitions"])
    for occurrence in hir["occurrences"]:
        assert occurrence["kind"] in OCCURRENCE_KINDS
        assert SEMANTIC.fullmatch(occurrence["target"])
    for resolution in hir["effectResolutions"]:
        require_object(resolution, ["ownerKind", "ownerIndex", "kind", "snapshotTarget"], "effect resolution")
        assert resolution["kind"] in ("atom", "parameter", "error")
        if resolution["kind"] == "parameter":
            assert re.fullmatch(r"effect-parameter:[0-9]+", resolution["snapshotTarget"] or "")
        else:
            assert resolution["snapshotTarget"] is None, "effect atom/error invented a target"

    for node in walk(data):
        if isinstance(node, dict) and set(("kind", "definition", "snapshotRef")) <= set(node):
            validate_type(node, semantic_ids)
        if (isinstance(node, dict) and set(("file", "start", "end")) <= set(node)
                and "byteSpan" not in node):
            assert node["file"] in lengths, f"span references unknown file {node['file']!r}"
            assert isinstance(node["start"], int) and isinstance(node["end"], int)
            assert 0 <= node["start"] <= node["end"] <= lengths[node["file"]]
        if isinstance(node, str) and (node.startswith("sem:") or re.match(r"^[a-z-]+:[0-9]", node)):
            assert SEMANTIC.fullmatch(node) or IDS.fullmatch(node) or node.startswith(("builtin:", "builtin-type:")), f"invalid reference {node!r}"

    types = artifacts["types"]
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("schema", type=Path)
    parser.add_argument("--require-generic", action="store_true")
    args = parser.parse_args()
    validate(args.output, args.schema, args.require_generic)


if __name__ == "__main__":
    main()
