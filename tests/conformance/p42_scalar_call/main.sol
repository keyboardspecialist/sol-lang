module conformance.p42_scalar_call

function helper() -> Int64 effects { pure } {
    return 7
}

@entry
public function launch() -> Int64 effects { pure } {
    return helper()
}
