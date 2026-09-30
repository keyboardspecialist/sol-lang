module conformance.p42_scalar_add_overflow

@entry
public function launch() -> Int64 effects { pure } {
    return 9223372036854775807 + 1
}
