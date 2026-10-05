module conformance.p44c_ensures_roots

@entry
public function first() -> Int64 effects { pure } ensures { result == 42 } {
    return 42
}

public function second() -> Bool effects { pure } ensures { result } {
    return true
}
