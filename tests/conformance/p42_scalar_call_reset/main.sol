module conformance.p42_scalar_call_reset

function leaf() -> Int64 effects { pure } {
    return 37
}

@entry
public function launch() -> Int64 effects { pure } {
    return leaf()
}
