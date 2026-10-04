module conformance.p44_require_unreachable

@entry
public function launch() -> Int64 effects { pure } {
    require false else { unreachable because { true } }
    return 42
}
