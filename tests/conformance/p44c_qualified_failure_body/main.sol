module conformance.p44c_qualified_failure_body

enum Failure { failed }

function checked() -> Result<Int64, Failure> effects { pure }
ensures { failure => false }
{
    let failed = 1 / 0
    return err(Failure.failed)
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = checked()
    return if value == err(Failure.failed) { 42 } else { 0 }
}
