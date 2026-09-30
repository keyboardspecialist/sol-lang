module conformance.p42_scalar_sub_overflow

@entry
public function launch() -> Int64 effects { pure } {
    let minimum = -9223372036854775807 - 1
    return minimum - 1
}
