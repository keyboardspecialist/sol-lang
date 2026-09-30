module conformance.p42_scalar_compound_div

@entry
public function launch() -> Int64 effects { pure } {
    var value = 8
    value /= 2
    return value
}
