module conformance.p44c_qualified_err_two_allocations

enum Failure { failed }

function checked(value: Int64, fail: Bool) -> Result<Int64, Failure>
effects { pure }
ensures {
    success => result / 0 > 0
    failure => old(value) == value
}
{
    return if fail { err(Failure.failed) } else { ok(value) }
}

@entry
public function launch() -> Int64 effects { pure } {
    let ignored = checked(42, true)
    return 42
}
