module conformance.p44c_qualified_callback_reject

enum Failure { failed }

function checked(value: Int64) -> Result<Int64, Failure> effects { pure }
ensures { success => result == value }
{
    return ok(value)
}

@entry
public function launch() -> Int64 effects { pure } {
    let callback = checked
    let value = callback(42)
    return if value == ok(42) { 42 } else { 0 }
}
