module conformance.p44c_ensures_current_two

function checked(first: Int64, second: Int64) -> Int64 effects { pure }
ensures { result == first - second } {
    return first - second
}

@entry
public function launch() -> Int64 effects { pure } {
    return checked(50, 8)
}
