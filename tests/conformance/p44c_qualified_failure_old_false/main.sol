module conformance.p44c_qualified_failure_old_false

enum Failure { failed }

function checked(first: Int64, second: Int64, fail: Bool) -> Result<Int64, Failure> effects { pure }
ensures { failure => first != old(first) || second != old(second) }
{
    return if fail { err(Failure.failed) } else { ok(first + second) }
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = checked(20, 22, true)
    return if value == err(Failure.failed) { 42 } else { 0 }
}
