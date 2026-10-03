module conformance.p43_callback_incompatible_signature

function increment(value: Int64) -> Int64 effects { pure } { return value + 1 }

@entry
public function launch() -> Int64 effects { pure } {
    let callback = increment
    return callback(41)
}
