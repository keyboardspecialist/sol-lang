module conformance.p44c_ensures_old_repeat

function checked(value: Int64) -> Int64 effects { pure }
ensures { old(value) == old(value) && result == old(value) } {
    return value
}

@entry
public function launch() -> Int64 effects { pure } {
    return checked(42)
}
