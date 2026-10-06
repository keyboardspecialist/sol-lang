module conformance.p44c_qualified_panic

enum Failure { failed }

function fail() -> Int64 effects { panic } {
    panic "qualified body panic"
}

function checked() -> Result<Int64, Failure> effects { panic }
ensures { failure => false }
{
    let ignored = fail()
    return if ignored == 0 { err(Failure.failed) } else { err(Failure.failed) }
}

@entry
public function launch() -> Int64 effects { panic } {
    let value = checked()
    return if value == err(Failure.failed) { 42 } else { 0 }
}
