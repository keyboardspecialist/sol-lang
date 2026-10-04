module conformance.p44_unreachable

@entry
public function launch() -> Int64 effects { pure } {
    unreachable because { true }
}
