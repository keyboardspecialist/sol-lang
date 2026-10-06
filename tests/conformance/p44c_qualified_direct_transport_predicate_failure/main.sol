module conformance.p44c_qualified_direct_transport

enum Failure { failed }

function inner(value: Int64, route: Int64) -> Result<Int64, Failure> effects { pure }
ensures {
    success => result == old(value) + 1 && old(value) / (route - 1) < 0
    failure => old(value) / route > 0
}
{
    if route == 3 {
        let failed = value / 0
        return ok(failed)
    } else {
        return if route == 1 {
            err(Failure.failed)
        } else {
            if route == 2 { ok(value + 2) } else { ok(value + 1) }
        }
    }
}

function outer(value: Int64, route: Int64) -> Result<Int64, Failure> effects { pure }
ensures {
    success => result == old(value) + 1 && old(value) / (route - 1) < 0
    failure => old(value) / route > 0
}
{
    let transported = inner(value, route)
    return transported
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = outer(41, 2)
    return if value == ok(43) { 42 } else { 0 }
}
