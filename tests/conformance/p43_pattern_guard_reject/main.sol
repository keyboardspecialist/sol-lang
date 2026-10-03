module conformance.p43_pattern_guard_reject

enum Choice {
    item(value: Bool),
}

@entry
public function launch() -> Int64 effects { pure } {
    return match Choice.item(true) {
        item(selected) if selected => 42,
        _ => 0,
    }
}
