module conformance.p44_require_true

@entry
public function launch() -> Int64 effects { panic } {
    require true else { panic "not reached" }
    return 42
}
