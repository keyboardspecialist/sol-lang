module conformance.p44e_depth_callback_inout

function decrement(value: inout Int64) -> () effects { pure } {
    if value > 0 {
        var next = value - 1
        let callback = decrement
        callback(next)
        value = next
    } else { () }
}

@entry
public function launch() -> Int64 effects { pure } {
    var value = 62
    let callback = decrement
    callback(value)
    return value + 42
}
public function fail() -> Int64 effects { pure } { return 43 }
