module conformance.p44c_qualified_old_false

enum Failure { failed }

function checked(value: Int64) -> Result<Int64, Failure> effects { pure }
ensures { success => result == old(value) }
{
    return ok(value + 1)
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = checked(41)
    return if value == ok(42) { 42 } else { 0 }
}
