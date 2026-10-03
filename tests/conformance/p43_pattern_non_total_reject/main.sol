module conformance.p43_pattern_non_total_reject

enum Choice {
    left(value: Bool),
    right(value: Bool),
}

@entry
public function launch() -> Int64 effects { pure } {
    return match Choice.left(true) {
        left(_) => 42,
    }
}
