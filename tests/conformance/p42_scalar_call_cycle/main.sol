module conformance.p42_scalar_call_cycle

function recur() -> Int64 effects { pure } {
    return recur()
}

@entry
public function launch() -> Int64 effects { pure } {
    return recur()
}
