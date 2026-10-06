module conformance.p44c_qualified_failure_old_eval

enum Failure { failed }

function checked(value: Int64, fail: Bool) -> Result<Int64, Failure> effects { pure }
ensures { failure => old(value) == value / 0 }
{
    return if fail { err(Failure.failed) } else { ok(value + 1) }
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = checked(41, true)
    return if value == err(Failure.failed) { 42 } else { 0 }
}
