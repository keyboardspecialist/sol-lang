module conformance.p44c_ensures_exclusive_reject

function increment(value: inout Int64) -> () effects { pure } {
    value += 1
}

@entry
public function launch() -> Int64 effects { pure } ensures { result == 42 } {
    var value = 41
    let callback = increment
    callback(value)
    return value
}
