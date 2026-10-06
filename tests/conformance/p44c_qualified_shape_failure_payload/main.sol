module conformance.p44c_qualified_shape_failure_payload

enum Failure { failed(code: Int64) }

function checked(fail: Bool) -> Result<Int64, Failure> effects { pure }
ensures { success => result == 42 }
{
    return if fail { err(Failure.failed(7)) } else { ok(42) }
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = checked(false)
    return if value == ok(42) { 42 } else { 0 }
}
