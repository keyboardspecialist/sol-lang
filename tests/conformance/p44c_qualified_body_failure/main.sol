module conformance.p44c_qualified_body_failure

enum Failure { failed }

function checked() -> Result<Int64, Failure> effects { pure }
ensures { success => false }
{
    let failed = 1 / 0
    return ok(failed)
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = checked()
    return if value == ok(42) { 42 } else { 0 }
}
