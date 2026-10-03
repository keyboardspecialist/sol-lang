module conformance.p43_callback_prereq

function increment(value: Int64) -> Int64 effects { pure } { return value + 1 }
function decrement(value: Int64) -> Int64 effects { pure } { return value - 1 }

@entry
public function launch() -> Int64 effects { pure } {
    let alternate = decrement
    let ignored = alternate(43)
    let callback = increment
    return callback(41)
}
