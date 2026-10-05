module conformance.p44c_ensures_false

@entry
public function launch() -> Int64 effects { pure } ensures { result == 42 } {
    return 41
}
