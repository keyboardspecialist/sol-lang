module conformance.p44c_qualified_constructor_allocation

enum Failure { failed }

function checked(value: Int64) -> Result<Int64, Failure>
effects { pure }
ensures { success => result == old(value) }
{
    return ok(value)
}

@entry
public function launch() -> Int64 effects { pure } {
    let ignored = checked(42)
    return 42
}
