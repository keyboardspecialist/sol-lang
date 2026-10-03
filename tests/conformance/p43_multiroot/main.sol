module conformance.p43_multiroot

@entry
public function first() -> Int64 effects { pure } {
    return 11
}

public function second() -> Int64 effects { pure } {
    return 29
}
