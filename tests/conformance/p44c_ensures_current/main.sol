module conformance.p44c_ensures_current

function checked(value: Int64) -> Int64 effects { pure } ensures { result == value + 1 } {
    return value + 1
}

@entry
public function launch() -> Int64 effects { pure } {
    return checked(42)
}
