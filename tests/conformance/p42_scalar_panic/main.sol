module conformance.p42_scalar_panic

@entry
public function launch() -> Int64 effects { panic } {
    panic "not supported by P4.2"
}
