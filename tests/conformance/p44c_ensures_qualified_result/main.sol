module conformance.p44c_ensures_qualified_result

enum Failure { failed }

function checked(value: Int64, fail: Bool) -> Result<Int64, Failure>
effects { pure }
ensures {
    success => result == value
    failure => value == old(value)
} {
    return if fail { err(Failure.failed) } else { ok(value) }
}

function checked_bool(value: Bool, fail: Bool) -> Result<Bool, Failure>
effects { pure }
ensures {
    success => result == value
    failure => true
} {
    return if fail { err(Failure.failed) } else { ok(value) }
}

function skipped_success() -> Result<Int64, Failure>
effects { pure }
ensures { success => result / 0 > 0 }
{
    return err(Failure.failed)
}

function skipped_failure() -> Result<Int64, Failure>
effects { pure }
ensures { failure => 1 / 0 > 0 }
{
    return ok(7)
}

@entry
public function launch() -> Int64 effects { pure } {
    let ok_value = checked(42, false)
    let err_value = checked(42, true)
    let ok_bool = checked_bool(true, false)
    let err_bool = checked_bool(false, true)
    let skipped_ok = skipped_success()
    let skipped_err = skipped_failure()
    return if ok_value == ok(42)
        && err_value == err(Failure.failed)
        && ok_bool == ok(true)
        && err_bool == err(Failure.failed)
        && skipped_ok == err(Failure.failed)
        && skipped_err == ok(7)
    { 42 } else { 0 }
}
