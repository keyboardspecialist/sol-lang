module relocation.composites

record Pair { left: Int64, right: Int64 }
enum Choice { yes(value: Int64), no }
capability Read { function read() -> Int64 effects { service.read<Self> } }

function identity<T>(value: T) -> T { return value }
function composite(value: Option<Int64>, choice: Choice, source: capability Read,
    provider: capability Read) -> Option<Int64>
    ensures { result == old(result) } {
    var local = -1
    local = identity<Int64>(Pair { left = 1, right = 2 }.left + 1)
    let selected = if true {
        match choice { yes(item) => item no => 0 }
    } else { 0 }
    let handled = handle service.read<source> with provider { source.read() }
    let propagated = value?
    panic "relocated panic"
    unreachable because { selected == local }
    require true else { panic "relocated fallback" }
    return some(selected + local + handled + propagated)
}

test "relocated test" match true { true => true false => false }
