module conformance.p44c_qualified_old_skipped

enum Failure { failed }

function checked(value: Int64, fail: Bool) -> Result<Int64, Failure> effects { pure }
ensures { success => old(value) / 0 > 0 }
{
    return if fail { err(Failure.failed) } else { ok(value) }
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = checked(7, true)
    return if value == err(Failure.failed) { 42 } else { 0 }
}
