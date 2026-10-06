module conformance.p44c_qualified_old_body_failure

enum Failure { failed }

function checked(value: Int64) -> Result<Int64, Failure> effects { pure }
ensures { success => result == old(value) }
{
    let failed = value / 0
    return ok(failed)
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = checked(42)
    return if value == ok(42) { 42 } else { 0 }
}
