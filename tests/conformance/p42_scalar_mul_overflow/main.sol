module conformance.p42_scalar_mul_overflow

@entry
public function launch() -> Int64 effects { pure } {
    return 9223372036854775807 * 2
}
