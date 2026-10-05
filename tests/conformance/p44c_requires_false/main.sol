module conformance.p44c_requires_false

@entry
public function launch() -> Int64 effects { pure } requires { false } {
    return 42
}
