module conformance.p44c_ensures_old_requires

function checked(value: Int64) -> Int64 effects { pure }
requires { value > 0 }
ensures { result == old(value) + 1 } {
    return value + 1
}

@entry
public function launch() -> Int64 effects { pure } {
    return checked(41)
}
