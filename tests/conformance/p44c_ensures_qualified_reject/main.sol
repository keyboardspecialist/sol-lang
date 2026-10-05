module conformance.p44c_ensures_qualified_reject

function checked() -> Result<Int64, Text> effects { pure } ensures {
    success => result == 42
    failure => true
} {
    return ok(42)
}

@entry
public function launch() -> Int64 effects { pure } {
    let value = checked()
    return if value == ok(42) { 42 } else { 0 }
}
