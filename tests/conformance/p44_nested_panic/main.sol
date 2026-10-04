module conformance.p44_nested_panic

private function inner() -> Int64 effects { panic } {
    panic "nested panic"
}

@entry
public function launch() -> Int64 effects { panic } {
    return inner()
}
