module conformance.p43_propagate

function advance(value: Option<Int64>) -> Option<Int64> effects { pure } {
    let item = value?
    return some(item + 1)
}

function adapt(value: Result<Int64, Text>) -> Result<Bool, Text> effects { pure } {
    let item = value?
    return ok(item == 7)
}

function unit_success(value: Option<()>) -> Option<()> effects { pure } {
    let item = value?
    return some(item)
}

@entry
public function launch() -> Int64 effects { pure } {
    let advanced = advance(some(41))
    let absent = advance(none())
    let adapted = adapt(ok(7))
    let failed = adapt(err("error"))
    let unit = unit_success(some(()))
    return if advanced == some(42) && absent == none() && adapted == ok(true)
        && failed == err("error") && unit == some(()) { 73 } else { 0 }
}
