module conformance.p44c_qualified_order

enum Failure { failed }

function checked(value: Int64, fail: Bool) -> Result<Int64, Failure>
effects { pure }
requires { value > 0 }
ensures {
    true
    success => result == value
    failure => value == old(value)
    success => result > 0
    failure => true
}
{
    return if fail { err(Failure.failed) } else { ok(value) }
}

@entry
public function launch() -> Int64 effects { pure } {
    let good = checked(42, false)
    let failed = checked(42, true)
    return if good == ok(42) && failed == err(Failure.failed) { 42 } else { 0 }
}
