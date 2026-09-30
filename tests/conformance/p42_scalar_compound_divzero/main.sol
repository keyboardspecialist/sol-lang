module conformance.p42_scalar_compound_divzero

@entry
public function launch() -> Int64 effects { pure } {
    var value = 8
    value /= 0
    return value
}
