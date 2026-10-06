module conformance.p44c_qualified_callee_failure

enum Failure { failed }

function fail() -> Int64 effects { pure } {
    unreachable because { true }
}

function checked() -> Result<Int64, Failure> effects { pure }
ensures {
    success => false
    failure => false
}
{
    return ok(fail())
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = checked()
    return if value == err(Failure.failed) { 42 } else { 0 }
}
