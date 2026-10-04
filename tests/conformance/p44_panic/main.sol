module conformance.p44_panic

@entry
public function launch() -> Int64 effects { panic } {
    panic "represented terminal panic"
}
