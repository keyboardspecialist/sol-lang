module conformance.p44c_requires_mul_overflow

@entry
public function launch() -> Int64 effects { pure } requires { 2 * 3 > 0 } {
    return 42
}

public function fail() -> Int64 effects { pure } requires { 3037000500 * 3037000500 > 0 } {
    return 42
}
