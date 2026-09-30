module conformance.p42_scalar_compound

@entry
public function launch() -> Int64 effects { pure } {
    var value = 40
    value += 2
    return value
}
