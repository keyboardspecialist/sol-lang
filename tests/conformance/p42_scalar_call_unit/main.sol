module conformance.p42_scalar_call_unit

function helper() -> () effects { pure } {
    return ()
}

@entry
public function launch() -> () effects { pure } {
    return helper()
}
