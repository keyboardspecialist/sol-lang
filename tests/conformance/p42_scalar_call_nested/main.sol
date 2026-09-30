module conformance.p42_scalar_call_nested

function leaf() -> Int64 effects { pure } {
    return 19
}

function middle() -> Int64 effects { pure } {
    return leaf()
}

@entry
public function launch() -> Int64 effects { pure } {
    return middle()
}
