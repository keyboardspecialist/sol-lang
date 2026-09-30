module conformance.p42_scalar_call_nested_divzero

function leaf() -> Int64 effects { pure } {
    return 8 / 0
}

function middle() -> Int64 effects { pure } {
    return leaf()
}

@entry
public function launch() -> Int64 effects { pure } {
    return middle()
}
