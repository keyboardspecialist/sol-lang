module conformance.p44_guarded_match

enum Choice {
    item(value: Bool),
}

@entry
public function launch() -> Int64 effects { pure } {
    return match Choice.item(false) {
        item(selected) if selected => 42,
        item(_) => 7,
    }
}
