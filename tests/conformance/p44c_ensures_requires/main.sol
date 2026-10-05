module conformance.p44c_ensures_requires

@entry
public function launch() -> Int64 effects { pure }
requires { true }
ensures { result == 42 } {
    return 42
}
