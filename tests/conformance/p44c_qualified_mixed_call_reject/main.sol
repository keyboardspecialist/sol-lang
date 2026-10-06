module conformance.p44c_qualified_mixed_call_reject

enum Failure { failed }

function checked(value: Int64) -> Result<Int64, Failure> effects { pure }
ensures { success => result == value }
{
    return ok(value)
}

@entry
public function launch() -> Int64 effects { pure } {
    let direct = checked(20)
    let callback = checked
    let indirect = callback(22)
    return if direct == ok(20) && indirect == ok(22) { 42 } else { 0 }
}
