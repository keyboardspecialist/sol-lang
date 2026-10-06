module conformance.p44c_qualified_multihop_failure

enum Failure { failed }

function checked() -> Result<Int64, Failure> effects { pure }
ensures { success => result / 0 > 0 }
{
    return ok(42)
}

function forward() -> Int64 effects { pure } {
    let value = checked()
    return if value == ok(42) { 42 } else { 0 }
}

@entry
public function launch() -> Int64 effects { pure } {
    return forward()
}
