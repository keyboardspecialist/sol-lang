module conformance.p44c_ensures_constant_unit

@entry
public function launch() -> () effects { pure } ensures { true } {
    return ()
}
