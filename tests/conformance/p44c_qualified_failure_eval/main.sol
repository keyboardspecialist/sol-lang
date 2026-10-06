module conformance.p44c_qualified_failure_eval

enum Failure { failed }

function checked(fail: Bool) -> Result<Int64, Failure> effects { pure }
ensures { failure => 1 / 0 > 0 }
{
    return if fail { err(Failure.failed) } else { ok(42) }
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = checked(true)
    return if value == err(Failure.failed) { 42 } else { 0 }
}
