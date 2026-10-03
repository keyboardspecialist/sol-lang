module conformance.p43_propagate_option_residual

function advance(value: Option<Int64>) -> Option<Int64> effects { pure } {
    let item = value?
    return some(item + 1)
}

@entry
public function launch() -> Int64 effects { pure } {
    return if advance(none()) == none() { 102 } else { 0 }
}
