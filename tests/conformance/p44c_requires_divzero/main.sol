module conformance.p44c_requires_divzero

@entry
public function launch() -> Int64 effects { pure } requires { 6 / 2 > 0 } {
    return 42
}

public function fail() -> Int64 effects { pure } requires { 1 / 0 > 0 } {
    return 42
}
