module conformance.p44c_ensures_old_false

function checked(value: Int64) -> Int64 effects { pure }
ensures { result == old(value) } {
    return value + 1
}

@entry
public function launch() -> Int64 effects { pure } {
    return checked(41)
}
