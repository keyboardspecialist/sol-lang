module conformance.p44c_ensures_old_body_failure

function checked(value: Int64) -> Int64 effects { pure }
ensures { result == old(value) } {
    return value / 0
}

@entry
public function launch() -> Int64 effects { pure } {
    return checked(42)
}
