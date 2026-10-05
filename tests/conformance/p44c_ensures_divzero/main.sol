module conformance.p44c_ensures_divzero

@entry
public function launch() -> Int64 effects { pure } ensures { result / 0 > 0 } {
    return 42
}
