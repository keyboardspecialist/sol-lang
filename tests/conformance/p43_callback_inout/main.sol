module conformance.p43_callback_inout

function increment(value: inout Int64) -> () effects { pure } {
    value += 1
}

@entry
public function launch() -> Int64 effects { pure } {
    var value = 41
    let callback = increment
    callback(value)
    return value
}

public function fail() -> Int64 effects { pure } {
    var value = 9223372036854775807
    let callback = increment
    callback(value)
    return value
}
