module conformance.p43_propagate_option_success

function advance(value: Option<Int64>) -> Option<Int64> effects { pure } {
    let item = value?
    return some(item + 1)
}

@entry
public function launch() -> Int64 effects { pure } {
    return if advance(some(41)) == some(42) { 101 } else { 0 }
}
