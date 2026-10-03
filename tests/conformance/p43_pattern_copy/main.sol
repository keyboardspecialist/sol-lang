module conformance.p43_pattern_copy

enum Payload {
    item(value: Text),
}

@entry
public function launch() -> Int64 effects { pure } {
    let source = Payload.item("pattern-copy")
    return match source {
        item(selected) => if selected == "pattern-copy" { 42 } else { 0 }
    }
}

function second() -> Int64 effects { pure } { return 0 }
