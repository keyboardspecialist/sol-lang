module conformance.p44c_ensures_body_failure

@entry
public function launch() -> Int64 effects { pure } ensures { false } {
    return 1 / 0
}
