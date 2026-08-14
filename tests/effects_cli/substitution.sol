module inspect.substitution

capability Reader {
    function read() -> Int64 effects { service.read<Self> }
    function copy(other: capability Reader) -> Int64 effects { service.read<other> }
    function both(other: borrow capability Reader) -> Int64
    effects { service.read<Self>, service.read<other> }
}

function consume(reader: capability Reader) -> Int64 effects { service.read<reader> } {
    return reader.read()
}

function call_consume(actual: capability Reader) -> Int64 effects { service.read<actual> } {
    return consume(actual)
}

function collapse(reader: capability Reader) -> Int64 effects { service.read<reader> } {
    return reader.both(reader)
}

function choose(
    source: capability Reader,
    left: capability Reader,
    right: capability Reader,
    flag: Bool,
) -> Int64 effects { service.read<left>, service.read<right> } {
    return source.copy(if flag { left } else { right })
}
