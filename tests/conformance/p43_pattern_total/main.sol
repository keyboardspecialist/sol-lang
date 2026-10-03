module conformance.p43_pattern_total

enum Choice {
    left(value: Bool),
    right(value: (Int64, Bool)),
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = Choice.right((7, true))
    return match value {
        right((_, flag)) => if flag { 42 } else { 0 },
        left(_) => 0,
    }
}
