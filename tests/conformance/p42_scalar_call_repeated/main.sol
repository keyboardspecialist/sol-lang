module conformance.p42_scalar_call_repeated

function helper() -> Int64 effects { pure } {
    return 23
}

@entry
public function launch() -> Int64 effects { pure } {
    helper()
    return helper()
}
