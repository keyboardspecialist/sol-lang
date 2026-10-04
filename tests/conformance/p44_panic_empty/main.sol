module conformance.p44_panic_empty

@entry
public function launch() -> Int64 effects { panic } {
    panic ""
}
