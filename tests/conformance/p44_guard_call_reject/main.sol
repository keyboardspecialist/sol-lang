module conformance.p44_guard_call_reject

enum Choice {
    item(value: Bool),
}

private function guard() -> Bool effects { pure } {
    return true
}

@entry
public function launch() -> Int64 effects { pure } {
    return match Choice.item(true) {
        item(_) if guard() => 42,
        item(_) => 7,
    }
}
