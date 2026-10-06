module conformance.p44c_ensures_current_repeat

function checked(value: Int64) -> Int64 effects { pure }
ensures { value == value && result == value } {
    return value
}

@entry
public function launch() -> Int64 effects { pure } {
    return checked(42)
}
