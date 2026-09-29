module conformance.p36_generic

record Box<T> {
    value: T,
}

function boxes() -> Bool effects { pure } {
    let integer = Box<Int64> { value = 1 }
    let text = Box<Text> { value = "box" }
    return true
}
