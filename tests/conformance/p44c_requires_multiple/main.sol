module conformance.p44c_requires_multiple

@entry
public function launch() -> Int64 effects { pure } requires { true && true } {
    return 42
}

public function fail() -> Int64 effects { pure } requires { false } {
    return 42
}
