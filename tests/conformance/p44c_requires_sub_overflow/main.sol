module conformance.p44c_requires_sub_overflow

@entry
public function launch() -> Int64 effects { pure } requires { 2 - 1 > 0 } {
    return 42
}

public function fail() -> Int64 effects { pure } requires { (-9223372036854775807 - 1) - 1 > 0 } {
    return 42
}
