module conformance.p44c_qualified_old_reverse

enum Failure { failed }

function checked(first: Int64, second: Int64, third: Int64) -> Result<Int64, Failure>
effects { pure }
ensures { success => result == old(first) + old(second) + old(third) }
{
    return ok(first + second + third)
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = checked(10, 20, 12)
    return if value == ok(42) { 42 } else { 0 }
}
