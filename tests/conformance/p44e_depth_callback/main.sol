module conformance.p44e_depth_callback

function descend(value: Int64) -> Int64 effects { pure } {
    if value == 0 { return 42 } else {
        let callback = descend
        return callback(value - 1)
    }
}

function decrement(value: inout Int64) -> () effects { pure } {
    if value > 0 {
        value -= 1
        let callback = decrement
        callback(value)
    } else { () }
}

@entry
public function launch() -> Int64 effects { pure } {
    let callback = descend
    return callback(62)
}
