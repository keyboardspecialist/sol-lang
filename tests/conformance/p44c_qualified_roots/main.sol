module conformance.p44c_qualified_roots

enum Failure { failed }

function checked(value: Int64) -> Result<Int64, Failure>
effects { pure }
ensures { success => result == value }
{
    return ok(value)
}

function checked_old(value: Int64) -> Result<Int64, Failure>
effects { pure }
ensures { success => result == old(value) }
{
    return ok(value)
}

@entry
public function first() -> Int64 effects { pure } {
    let current = checked(42)
    return if current == ok(42) { 42 } else { 0 }
}

function second() -> Int64 effects { pure } {
    let prior = checked_old(43)
    return if prior == ok(43) { 43 } else { 0 }
}
