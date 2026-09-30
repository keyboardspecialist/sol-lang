module conformance.p42_scalar_rem_success

@entry
public function launch() -> Int64 effects { pure } {
    return -9 % 4
}
