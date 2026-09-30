module conformance.p42_scalar_call_leaf_divzero

function leaf(value: Int64) -> Int64 effects { pure } {
    return value / 0
}

@entry
public function launch() -> Int64 effects { pure } {
    var survivor = 1
    return leaf(1)
}
