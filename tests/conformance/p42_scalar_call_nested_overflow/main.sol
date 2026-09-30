module conformance.p42_scalar_call_nested_overflow

function leaf() -> Int64 effects { pure } {
    return 9223372036854775807 + 1
}

function middle() -> Int64 effects { pure } {
    return leaf()
}

@entry
public function launch() -> Int64 effects { pure } {
    return middle()
}
