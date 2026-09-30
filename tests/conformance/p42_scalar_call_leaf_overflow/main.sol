module conformance.p42_scalar_call_leaf_overflow

function leaf(value: Int64) -> Int64 effects { pure } {
    return 9223372036854775807 + value
}

@entry
public function launch() -> Int64 effects { pure } {
    var survivor = 1
    return leaf(1)
}
