module conformance.p44c_qualified_shape_multi_failure

enum Failure { first, second }

function checked(fail: Bool) -> Result<Int64, Failure> effects { pure }
ensures { success => result == 42 }
{
    return if fail { err(Failure.second) } else { ok(42) }
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = checked(false)
    return if value == ok(42) { 42 } else { 0 }
}
