module conformance.p44c_requires_div_overflow

@entry
public function launch() -> Int64 effects { pure } requires { 6 / 2 > 0 } {
    return 42
}

public function fail() -> Int64 effects { pure } requires { (-9223372036854775807 - 1) / -1 > 0 } {
    return 42
}
