module conformance.p44c_requires_true

@entry
public function launch() -> Int64 effects { pure } requires { true } {
    return 42
}
