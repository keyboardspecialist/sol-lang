module conformance.p44c_qualified_always_false

enum Failure { failed }

function checked() -> Result<Int64, Failure> effects { pure }
ensures {
    false
    success => result / 0 > 0
}
{
    return ok(42)
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = checked()
    return if value == ok(42) { 42 } else { 0 }
}
