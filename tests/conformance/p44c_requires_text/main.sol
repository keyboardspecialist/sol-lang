module conformance.p44c_requires_text

@entry
public function launch() -> Int64 effects { pure } requires { "represented" == "represented" } {
    return 42
}
