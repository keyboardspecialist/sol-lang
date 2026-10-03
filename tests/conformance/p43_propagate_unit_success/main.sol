module conformance.p43_propagate_unit_success

function transfer(value: Option<()>) -> Option<()> effects { pure } {
    let item = value?
    return some(item)
}

@entry
public function launch() -> Int64 effects { pure } {
    return if transfer(some(())) == some(()) { 105 } else { 0 }
}
