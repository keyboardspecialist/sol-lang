module conformance.p44c_requires_short_circuit

@entry
public function launch() -> Int64 effects { pure } requires { true || (1 / 0 > 0) } {
    return 42
}

public function fail() -> Int64 effects { pure } requires { false } {
    return 42
}
