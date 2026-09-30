module conformance.p42_scalar_remzero

@entry
public function launch() -> Int64 effects { pure } {
    return 8 % 0
}
