module conformance.p44c_qualified_failure_old_skipped

enum Failure { failed }

function checked(value: Int64) -> Result<Int64, Failure> effects { pure }
ensures { failure => old(value) / 0 > 0 }
{
    return ok(value + 1)
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = checked(41)
    return if value == ok(42) { 42 } else { 0 }
}
