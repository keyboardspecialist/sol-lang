module conformance.p44c_qualified_payload_edges

enum Failure { failed }

function negative() -> Result<Int64, Failure> effects { pure }
ensures { success => result < 0 }
{
    return ok(-42)
}

function false_bool() -> Result<Bool, Failure> effects { pure }
ensures { success => !result }
{
    return ok(false)
}

@entry
public function launch() -> Int64 effects { pure } {
    let integer = negative()
    let boolean = false_bool()
    return if integer == ok(-42) && boolean == ok(false) { 42 } else { 0 }
}
