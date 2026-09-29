module conformance.p36_copy

function copy_text(value: Text) -> Bool effects { pure } {
    let duplicate = value
    return duplicate == value
}
