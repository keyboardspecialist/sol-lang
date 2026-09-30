module conformance.p42_scalar_compound_overflow

@entry
public function launch() -> Int64 effects { pure } {
    var value = 9223372036854775807
    value += 1
    return value
}
