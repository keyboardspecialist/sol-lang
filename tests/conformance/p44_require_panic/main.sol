module conformance.p44_require_panic

@entry
public function launch() -> Int64 effects { panic } {
    require false else { panic "require fallback" }
    return 42
}
