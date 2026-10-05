module conformance.p44c_ensures_computed_false

@entry
public function launch() -> Int64 effects { pure } ensures { result == 42 } {
    let value = 40
    return value + 1
}
